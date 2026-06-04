#include "vterm.h"

#include <stdlib.h>
#include <string.h>

#define PROMPT "ds4-agent>"

struct pin_vterm {
    int cols, rows;
    char **grid;        /* rows x (cols), space-filled, not NUL-terminated */
    int cr, cc;         /* cursor row/col */
    pin_buf scrollback; /* finalized rows pushed off the top, '\n' per row */

    /* escape state: 0 = normal, 1 = saw ESC, 2 = in CSI body */
    int esc;
    char csi[64];
    size_t csi_len;
};

static void row_clear(pin_vterm *vt, int r) {
    memset(vt->grid[r], ' ', (size_t)vt->cols);
}

pin_vterm *pin_vterm_new(int cols, int rows) {
    if (cols < 8) cols = 8;
    if (rows < 3) rows = 3;
    pin_vterm *vt = calloc(1, sizeof(*vt));
    if (!vt) return NULL;
    vt->cols = cols;
    vt->rows = rows;
    vt->grid = calloc((size_t)rows, sizeof(char *));
    for (int r = 0; r < rows; r++) {
        vt->grid[r] = malloc((size_t)cols);
        memset(vt->grid[r], ' ', (size_t)cols);
    }
    pin_buf_init(&vt->scrollback);
    return vt;
}

void pin_vterm_free(pin_vterm *vt) {
    if (!vt) return;
    for (int r = 0; r < vt->rows; r++) free(vt->grid[r]);
    free(vt->grid);
    pin_buf_free(&vt->scrollback);
    free(vt);
}

/* True for rows that belong to the editor widget (the prompt line or the
 * status footer), which linenoise redraws constantly and which scroll off
 * the top as content grows. They must never be treated as content. */
static bool is_editor_row(const char *row, int cols) {
    if (cols >= (int)strlen(PROMPT) && memcmp(row, PROMPT, strlen(PROMPT)) == 0)
        return true;
    char tmp[256];
    int n = cols < (int)sizeof(tmp) - 1 ? cols : (int)sizeof(tmp) - 1;
    memcpy(tmp, row, (size_t)n);
    tmp[n] = '\0';
    if (strstr(tmp, "ctx ") != NULL && strstr(tmp, "| ") != NULL) return true;
    /* one-time boot banner ds4-agent prints after the model loads */
    if (strstr(tmp, "DwarfStar Agent, context") != NULL) return true;
    return false;
}

/* Append a row's visible text (trailing spaces trimmed) + '\n', unless it
 * is an editor-widget row (then it is dropped) or an exact duplicate of
 * the previous scrollback line (linenoise re-emits rows during redraws). */
static void push_row_to_scrollback(pin_vterm *vt, int r) {
    if (is_editor_row(vt->grid[r], vt->cols)) return;
    int end = vt->cols;
    while (end > 0 && vt->grid[r][end - 1] == ' ') end--;
    if (end == 0) return;            /* drop blank scrolled rows */
    /* de-dup against the previous scrollback line */
    size_t sl = vt->scrollback.len;
    if (sl >= (size_t)end + 1) {
        const char *prev = vt->scrollback.ptr + sl - (size_t)end - 1;
        if (prev[(size_t)end] == '\n' &&
            (sl == (size_t)end + 1 || prev[-1] == '\n') &&
            memcmp(prev, vt->grid[r], (size_t)end) == 0)
            return;
    }
    pin_buf_append(&vt->scrollback, vt->grid[r], (size_t)end);
    pin_buf_putc(&vt->scrollback, '\n');
}

static void scroll_up(pin_vterm *vt) {
    push_row_to_scrollback(vt, 0);
    char *first = vt->grid[0];
    for (int r = 0; r < vt->rows - 1; r++) vt->grid[r] = vt->grid[r + 1];
    vt->grid[vt->rows - 1] = first;
    row_clear(vt, vt->rows - 1);
}

static void do_lf(pin_vterm *vt) {
    if (vt->cr == vt->rows - 1) scroll_up(vt);
    else vt->cr++;
}

static void put_byte(pin_vterm *vt, unsigned char b) {
    if (vt->cc >= vt->cols) vt->cc = vt->cols - 1;
    vt->grid[vt->cr][vt->cc] = (char)b;
    vt->cc++;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Apply a CSI sequence: csi holds the bytes between ESC[ and the final. */
static int apply_csi(pin_vterm *vt, const char *body, size_t len) {
    if (len == 0) return 0;
    char final = body[len - 1];
    /* private sequences (?...) are modes we ignore */
    if (body[0] == '?') return final == 'n' ? 1 : 0; /* (no private CPR) */
    int n = 0;
    bool have = false;
    int nums[8];
    int nn = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        char c = body[i];
        if (c >= '0' && c <= '9') { n = n * 10 + (c - '0'); have = true; }
        else if (c == ';') { if (nn < 8) nums[nn++] = have ? n : 0; n = 0; have = false; }
    }
    if (nn < 8) nums[nn++] = have ? n : 0;
    int p = nums[0];
    switch (final) {
        case 'A': vt->cr = clampi(vt->cr - (p ? p : 1), 0, vt->rows - 1); break;
        case 'B': vt->cr = clampi(vt->cr + (p ? p : 1), 0, vt->rows - 1); break;
        case 'C': vt->cc = clampi(vt->cc + (p ? p : 1), 0, vt->cols - 1); break;
        case 'D': vt->cc = clampi(vt->cc - (p ? p : 1), 0, vt->cols - 1); break;
        case 'G': vt->cc = clampi((p ? p : 1) - 1, 0, vt->cols - 1); break;
        case 'H': case 'f': {
            int row = nn >= 1 ? nums[0] : 1;
            int col = nn >= 2 ? nums[1] : 1;
            vt->cr = clampi((row ? row : 1) - 1, 0, vt->rows - 1);
            vt->cc = clampi((col ? col : 1) - 1, 0, vt->cols - 1);
            break;
        }
        case 'K':
            if (p == 0) for (int x = vt->cc; x < vt->cols; x++) vt->grid[vt->cr][x] = ' ';
            else if (p == 1) for (int x = 0; x <= vt->cc && x < vt->cols; x++) vt->grid[vt->cr][x] = ' ';
            else if (p == 2) row_clear(vt, vt->cr);
            break;
        case 'J':
            if (p == 2) { for (int r = 0; r < vt->rows; r++) row_clear(vt, r); vt->cr = vt->cc = 0; }
            break;
        case 'n':
            if (p == 6) return 1; /* CPR request */
            break;
        default: break;            /* m (SGR) and anything else: ignore */
    }
    return 0;
}

int pin_vterm_feed(pin_vterm *vt, const char *bytes, size_t n) {
    int cpr = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)bytes[i];
        if (vt->esc == 1) {
            if (b == '[') { vt->esc = 2; vt->csi_len = 0; }
            else vt->esc = 0;       /* ESC + non-[ : ignore */
            continue;
        }
        if (vt->esc == 2) {
            if (vt->csi_len < sizeof(vt->csi)) vt->csi[vt->csi_len++] = (char)b;
            if (b >= 0x40 && b <= 0x7E) {
                cpr += apply_csi(vt, vt->csi, vt->csi_len);
                vt->esc = 0;
            }
            continue;
        }
        if (b == 0x1B) { vt->esc = 1; continue; }
        if (b == 0x0D) vt->cc = 0;
        else if (b == 0x0A) do_lf(vt);
        else if (b == 0x08) { if (vt->cc > 0) vt->cc--; }
        else if (b == 0x09) vt->cc = clampi((vt->cc / 8 + 1) * 8, 0, vt->cols - 1);
        else if (b >= 0x20) put_byte(vt, b);
    }
    return cpr;
}

/* Index of the editor widget's top (the prompt row), or rows if none. */
static int prompt_row(pin_vterm *vt) {
    size_t plen = strlen(PROMPT);
    for (int r = vt->rows - 1; r >= 0; r--) {
        if ((size_t)vt->cols >= plen && memcmp(vt->grid[r], PROMPT, plen) == 0)
            return r;
    }
    return vt->rows;
}

void pin_vterm_content(pin_vterm *vt, pin_buf *out) {
    if (!vt || !out) return;
    pin_buf_append(out, vt->scrollback.ptr, vt->scrollback.len);
    int pr = prompt_row(vt);
    for (int r = 0; r < pr; r++) {
        if (is_editor_row(vt->grid[r], vt->cols)) continue;
        int end = vt->cols;
        while (end > 0 && vt->grid[r][end - 1] == ' ') end--;
        if (end == 0) continue;
        pin_buf_append(out, vt->grid[r], (size_t)end);
        pin_buf_putc(out, '\n');
    }
}

void pin_vterm_status(pin_vterm *vt, char *buf, size_t cap) {
    if (!vt || !buf || !cap) return;
    buf[0] = '\0';
    for (int r = vt->rows - 1; r >= 0; r--) {
        /* row contains "ctx <...> | <state> ..." */
        char line[512];
        int end = vt->cols < (int)sizeof(line) - 1 ? vt->cols : (int)sizeof(line) - 1;
        memcpy(line, vt->grid[r], (size_t)end);
        line[end] = '\0';
        const char *ctx = strstr(line, "ctx ");
        const char *bar = strstr(line, "| ");
        if (ctx && bar && bar > ctx) {
            const char *st = bar + 2;
            while (*st == ' ') st++;
            size_t i = 0;
            while (st[i] && st[i] != ' ' && i + 1 < cap) { buf[i] = st[i]; i++; }
            buf[i] = '\0';
            return;
        }
    }
}
