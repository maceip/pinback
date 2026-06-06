/* fake-kv-ds4-agent: TUI-mode stand-in for ds4-agent when LINENOISE_ASSUME_TTY=1.
 *
 * Exercises pinback's --kv-resume path without a real model:
 *   - Emits linenoise-style status rows (ctx … | idle) for pin_vterm.
 *   - Honors /save (writes <kvcache>/<sha>.kv, prints saved session …).
 *   - Honors /switch (prints session history block + switched to session …).
 *   - User prompts get a short prose reply on stdout + idle status.
 *   - Optional --trace writes generation tokens for pin_tracestream.
 *
 * Set PINBACK_TEST_KVCACHE to the directory for .kv files (tests create it).
 */

#include "../src/util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *g_kvcache = "";
static const char *g_trace = NULL;
static unsigned g_save_ctr;

static void emit_status_idle(void)
{
    fputs("ds4-agent> \r\n", stdout);
    fputs("ctx 0/8192 | idle\r\n", stdout);
    fflush(stdout);
}

static void emit_status_busy(void)
{
    fputs("ds4-agent> \r\n", stdout);
    fputs("ctx 100/8192 | generation 4 tokens 120 t/s\r\n", stdout);
    fflush(stdout);
}

static void trace_token(const char *text)
{
    if (!g_trace || !text)
        return;
    FILE *f = fopen(g_trace, "a");
    if (!f)
        return;
    fprintf(f, "token index=0 id=0 bytes=%zu text=\"", strlen(text));
    for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\')
            fputc('\\', f);
        fputc(*p, f);
    }
    fputs("\" hex=\n", f);
    fclose(f);
}

static void trace_turn(const char *user, const char *answer)
{
    if (!g_trace)
        return;
    FILE *f = fopen(g_trace, "a");
    if (!f)
        return;
    fputs("tokens label=prefill_suffix start=0 len=1\n", f);
    fclose(f);
    trace_token(answer);
}

static void write_kv(const char *sha)
{
    if (!g_kvcache[0])
        return;
    mkdir(g_kvcache, 0700);
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s.kv", g_kvcache, sha);
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs("fake-kv\n", f);
        fclose(f);
    }
}

static void handle_save(void)
{
    char sha[16];
    snprintf(sha, sizeof(sha), "%08x", ++g_save_ctr);
    write_kv(sha);
    emit_status_busy();
    printf("saved session %s (42 tokens)\r\n", sha);
    emit_status_idle();
    fflush(stdout);
}

static void handle_switch(const char *prefix)
{
    emit_status_busy();
    fputs("--- session history: last 1 user turns ---\r\n", stdout);
    fputs("User: (restored from KV)\r\n", stdout);
    fputs("--- end history ---\r\n", stdout);
    printf("switched to session %s (42 tokens)\r\n", prefix);
    emit_status_idle();
    fflush(stdout);
}

static void handle_prompt(const char *line)
{
    (void)line;
    emit_status_busy();
    printf("echo: %s\r\n\r\n", line);
    trace_turn(line, "echo-ok");
    emit_status_idle();
    fflush(stdout);
}

static void read_line_cr(pin_buf *acc)
{
    for (;;) {
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
        int pr = poll(&pfd, 1, acc->len ? 80 : -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0 && acc->len) {
            char *p = acc->ptr;
            size_t len = acc->len;
            while (len && (p[len - 1] == '\r' || p[len - 1] == '\n' || p[len - 1] == ' '))
                len--;
            char *cmd = pin_xstrndup(p, len);
            if (!strcmp(cmd, "/quit") || !strcmp(cmd, "/exit")) {
                free(cmd);
                exit(0);
            }
            if (!strncmp(cmd, "/save", 5))
                handle_save();
            else if (!strncmp(cmd, "/switch ", 8))
                handle_switch(cmd + 8);
            else
                handle_prompt(cmd);
            free(cmd);
            pin_buf_clear(acc);
            return;
        }
        if (pr > 0) {
            char b[256];
            ssize_t n = read(STDIN_FILENO, b, sizeof(b));
            if (n <= 0)
                break;
            pin_buf_append(acc, b, (size_t)n);
        }
    }
}

int main(int argc, char **argv)
{
    const char *trace = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace") && i + 1 < argc)
            trace = argv[++i];
        else if (!strcmp(argv[i], "--chdir") && i + 1 < argc)
            (void)argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc)
            (void)argv[++i];
    }
    g_trace = trace;
    if (trace && trace[0]) {
        FILE *f = fopen(trace, "wb");
        if (f)
            fclose(f);
    }
    const char *kc = getenv("PINBACK_TEST_KVCACHE");
    if (kc && *kc)
        g_kvcache = kc;

    if (!getenv("LINENOISE_ASSUME_TTY")) {
        fprintf(stderr, "fake-kv-ds4-agent: set LINENOISE_ASSUME_TTY=1\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Boot: sysprompt prefill (busy) then idle prompt — pinback waits for this
     * before /switch so the load is not overwritten by prefill. */
    emit_status_busy();
    emit_status_idle();

    pin_buf acc;
    pin_buf_init(&acc);
    for (;;)
        read_line_cr(&acc);
}
