#include "workspace.h"

#include "log.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Internal record holds the meta plus a hot pin_event_log handle. */
typedef struct {
    pin_workspace_meta meta;
    pin_event_log *log; /* lazy-opened */
} ws_rec;

struct pin_workspace_store {
    pthread_rwlock_t rw;
    char *root;        /* e.g. /Users/x/.pinback */
    char *catalog;     /* root/workspaces.json */
    char *active_file; /* root/active_id */
    size_t per_ws_ring_cap;

    ws_rec **records;
    size_t count;
    size_t cap;

    char *active_id;
};

/* ====================================================================
 * Filesystem helpers
 * ==================================================================== */

static bool ensure_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0)
        return true;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return true;
    }
    return false;
}

static bool ensure_dir_recursive(const char *path, mode_t mode)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t n = strlen(tmp);
    if (n == 0)
        return false;
    if (tmp[n - 1] == '/')
        tmp[--n] = '\0';
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (!ensure_dir(tmp, mode)) {
                if (errno != EEXIST)
                    return false;
            }
            tmp[i] = '/';
        }
    }
    return ensure_dir(tmp, mode);
}

/* Atomic write: write to <path>.tmp then rename. */
static bool atomic_write(const char *path, const char *data, size_t len)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd < 0)
        return false;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmp);
            return false;
        }
        off += (size_t)w;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static char *slurp(const char *path, size_t *out_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    size_t cap = (size_t)st.st_size;
    char *buf = pin_xmalloc(cap + 1);
    size_t got = 0;
    while (got < cap) {
        ssize_t n = read(fd, buf + got, cap - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            free(buf);
            return NULL;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    buf[got] = '\0';
    if (out_len)
        *out_len = got;
    return buf;
}

/* ====================================================================
 * JSON serializers
 * ==================================================================== */

static void emit_meta_json(pin_buf *b, const pin_workspace_meta *m)
{
    pin_buf_putc(b, '{');
    pin_buf_puts(b, "\"id\":");
    pin_json_str(b, m->id ? m->id : "");
    pin_buf_puts(b, ",\"path\":");
    pin_json_str(b, m->path ? m->path : "");
    pin_buf_puts(b, ",\"label\":");
    pin_json_str(b, m->label ? m->label : "");
    pin_buf_puts(b, ",\"session_sha\":");
    if (m->session_sha)
        pin_json_str(b, m->session_sha);
    else
        pin_buf_puts(b, "null");
    pin_buf_printf(b, ",\"created_ms\":%llu", (unsigned long long)m->created_ms);
    pin_buf_printf(b, ",\"last_active_ms\":%llu", (unsigned long long)m->last_active_ms);
    pin_buf_putc(b, '}');
}

static void persist_catalog(pin_workspace_store *s)
{
    pin_buf b;
    pin_buf_init(&b);
    pin_buf_puts(&b, "{\"version\":1,\"active_id\":");
    if (s->active_id)
        pin_json_str(&b, s->active_id);
    else
        pin_buf_puts(&b, "null");
    pin_buf_puts(&b, ",\"workspaces\":[");
    for (size_t i = 0; i < s->count; i++) {
        if (i)
            pin_buf_putc(&b, ',');
        emit_meta_json(&b, &s->records[i]->meta);
    }
    pin_buf_puts(&b, "]}\n");
    if (!atomic_write(s->catalog, b.ptr, b.len)) {
        PIN_LOG_WARNF("workspace.persist_fail", "path=%s errno=%d", s->catalog, errno);
    }
    pin_buf_free(&b);
}

static void persist_meta(pin_workspace_store *s, const pin_workspace_meta *m)
{
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/workspaces/%s", s->root, m->id);
    ensure_dir_recursive(dir, 0700);
    char path[4096];
    snprintf(path, sizeof(path), "%s/meta.json", dir);
    pin_buf b;
    pin_buf_init(&b);
    emit_meta_json(&b, m);
    pin_buf_putc(&b, '\n');
    atomic_write(path, b.ptr, b.len);
    pin_buf_free(&b);
}

/* ====================================================================
 * Record helpers
 * ==================================================================== */

static void rec_free(ws_rec *r)
{
    if (!r)
        return;
    if (r->log)
        pin_event_log_close(r->log);
    pin_workspace_meta_free(&r->meta);
    free(r);
}

void pin_workspace_meta_free(pin_workspace_meta *m)
{
    if (!m)
        return;
    free(m->id);
    m->id = NULL;
    free(m->path);
    m->path = NULL;
    free(m->label);
    m->label = NULL;
    free(m->session_sha);
    m->session_sha = NULL;
}

void pin_workspace_meta_array_free(pin_workspace_meta *arr, size_t n)
{
    if (!arr)
        return;
    for (size_t i = 0; i < n; i++)
        pin_workspace_meta_free(&arr[i]);
    free(arr);
}

static void records_push(pin_workspace_store *s, ws_rec *r)
{
    if (s->count + 1 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 8;
        s->records = pin_xrealloc(s->records, nc * sizeof(ws_rec *));
        s->cap = nc;
    }
    s->records[s->count++] = r;
}

static ws_rec *find_rec_locked(pin_workspace_store *s, const char *id)
{
    if (!id)
        return NULL;
    for (size_t i = 0; i < s->count; i++) {
        if (s->records[i]->meta.id && !strcmp(s->records[i]->meta.id, id))
            return s->records[i];
    }
    return NULL;
}

/* ====================================================================
 * Catalog parser (narrow JSON)
 * ==================================================================== */

static char *parse_string_field(const char *obj, const char *key)
{
    const char *vp = NULL;
    if (!pin_json_find_key(obj, key, &vp))
        return NULL;
    pin_json_ws(&vp);
    if (*vp == 'n')
        return NULL; /* null */
    char *out = NULL;
    if (!pin_json_parse_string(&vp, &out))
        return NULL;
    return out;
}

static long long parse_int_field(const char *obj, const char *key)
{
    const char *vp = NULL;
    long long v = 0;
    if (!pin_json_find_key(obj, key, &vp))
        return 0;
    if (!pin_json_parse_int(&vp, &v))
        return 0;
    return v;
}

static void load_catalog(pin_workspace_store *s)
{
    size_t n = 0;
    char *raw = slurp(s->catalog, &n);
    if (!raw)
        return;

    char *active = parse_string_field(raw, "active_id");
    if (active) {
        free(s->active_id);
        s->active_id = active;
    }

    /* Find the workspaces array. */
    const char *vp = NULL;
    if (!pin_json_find_key(raw, "workspaces", &vp)) {
        free(raw);
        return;
    }
    pin_json_ws(&vp);
    if (*vp != '[') {
        free(raw);
        return;
    }
    vp++;
    pin_json_ws(&vp);
    while (*vp && *vp != ']') {
        pin_json_ws(&vp);
        if (*vp != '{')
            break;
        const char *obj_start = vp;
        if (!pin_json_skip_value(&vp))
            break;
        size_t obj_len = (size_t)(vp - obj_start);
        char *obj = pin_xstrndup(obj_start, obj_len);

        ws_rec *r = pin_xcalloc(1, sizeof(*r));
        r->meta.id = parse_string_field(obj, "id");
        r->meta.path = parse_string_field(obj, "path");
        r->meta.label = parse_string_field(obj, "label");
        r->meta.session_sha = parse_string_field(obj, "session_sha");
        r->meta.created_ms = (uint64_t)parse_int_field(obj, "created_ms");
        r->meta.last_active_ms = (uint64_t)parse_int_field(obj, "last_active_ms");
        free(obj);

        if (r->meta.id && r->meta.path) {
            records_push(s, r);
        } else {
            rec_free(r);
        }
        pin_json_ws(&vp);
        if (*vp == ',') {
            vp++;
            pin_json_ws(&vp);
        }
    }
    free(raw);
}

/* ====================================================================
 * Public API
 * ==================================================================== */

pin_workspace_store *pin_workspace_store_open(const char *root_dir, size_t per_ws_ring_cap)
{
    if (!root_dir || !*root_dir)
        return NULL;
    if (per_ws_ring_cap == 0)
        per_ws_ring_cap = 4096;
    if (!ensure_dir_recursive(root_dir, 0700)) {
        PIN_LOG_ERRF("workspace.root_fail", "root=%s errno=%d", root_dir, errno);
        return NULL;
    }
    char ws_root[4096];
    snprintf(ws_root, sizeof(ws_root), "%s/workspaces", root_dir);
    ensure_dir(ws_root, 0700);

    pin_workspace_store *s = pin_xcalloc(1, sizeof(*s));
    pthread_rwlock_init(&s->rw, NULL);
    s->root = pin_xstrdup(root_dir);
    s->per_ws_ring_cap = per_ws_ring_cap;

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s/workspaces.json", root_dir);
    s->catalog = pin_xstrdup(tmp);
    snprintf(tmp, sizeof(tmp), "%s/active_id", root_dir);
    s->active_file = pin_xstrdup(tmp);

    load_catalog(s);
    return s;
}

void pin_workspace_store_close(pin_workspace_store *s)
{
    if (!s)
        return;
    pthread_rwlock_wrlock(&s->rw);
    for (size_t i = 0; i < s->count; i++)
        rec_free(s->records[i]);
    free(s->records);
    free(s->active_id);
    free(s->root);
    free(s->catalog);
    free(s->active_file);
    pthread_rwlock_unlock(&s->rw);
    pthread_rwlock_destroy(&s->rw);
    free(s);
}

static void copy_meta(pin_workspace_meta *dst, const pin_workspace_meta *src)
{
    memset(dst, 0, sizeof(*dst));
    if (src->id)
        dst->id = pin_xstrdup(src->id);
    if (src->path)
        dst->path = pin_xstrdup(src->path);
    if (src->label)
        dst->label = pin_xstrdup(src->label);
    if (src->session_sha)
        dst->session_sha = pin_xstrdup(src->session_sha);
    dst->created_ms = src->created_ms;
    dst->last_active_ms = src->last_active_ms;
}

pin_workspace_meta *pin_workspace_store_list(pin_workspace_store *s, size_t *out_count)
{
    if (out_count)
        *out_count = 0;
    if (!s)
        return NULL;
    pthread_rwlock_rdlock(&s->rw);
    pin_workspace_meta *arr = NULL;
    if (s->count > 0) {
        arr = pin_xcalloc(s->count, sizeof(*arr));
        for (size_t i = 0; i < s->count; i++) {
            copy_meta(&arr[i], &s->records[i]->meta);
        }
        if (out_count)
            *out_count = s->count;
    }
    pthread_rwlock_unlock(&s->rw);
    return arr;
}

bool pin_workspace_store_get(pin_workspace_store *s, const char *id, pin_workspace_meta *out)
{
    if (!s || !id || !out)
        return false;
    pthread_rwlock_rdlock(&s->rw);
    ws_rec *r = find_rec_locked(s, id);
    bool ok = false;
    if (r) {
        copy_meta(out, &r->meta);
        ok = true;
    }
    pthread_rwlock_unlock(&s->rw);
    return ok;
}

static char *make_id(void)
{
    char hex[17];
    pin_random_hex(hex, sizeof(hex), 8);
    char buf[32];
    snprintf(buf, sizeof(buf), "ws_%s", hex);
    return pin_xstrdup(buf);
}

static char *basename_dup(const char *path)
{
    if (!path)
        return pin_xstrdup("workspace");
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    if (!*base)
        base = "workspace";
    return pin_xstrdup(base);
}

bool pin_workspace_store_create(pin_workspace_store *s, const char *path, const char *label,
                                char **out_id, char **out_err)
{
    if (out_id)
        *out_id = NULL;
    if (out_err)
        *out_err = NULL;
    if (!s || !path || !*path) {
        if (out_err)
            *out_err = pin_xstrdup("path required");
        return false;
    }
    if (path[0] != '/') {
        if (out_err)
            *out_err = pin_xstrdup("path must be absolute");
        return false;
    }
    /* Reject embedded NUL or .. for safety. */
    if (strstr(path, "/..") || strstr(path, "../")) {
        if (out_err)
            *out_err = pin_xstrdup("path must not contain '..'");
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (out_err)
            *out_err = pin_xstrdup("path is not a directory");
        return false;
    }

    pthread_rwlock_wrlock(&s->rw);
    /* Reject duplicate paths to keep the catalog clean. */
    for (size_t i = 0; i < s->count; i++) {
        if (s->records[i]->meta.path && strcmp(s->records[i]->meta.path, path) == 0) {
            pthread_rwlock_unlock(&s->rw);
            if (out_err)
                *out_err = pin_xstrdup("workspace already exists for this path");
            return false;
        }
    }
    ws_rec *r = pin_xcalloc(1, sizeof(*r));
    r->meta.id = make_id();
    r->meta.path = pin_xstrdup(path);
    r->meta.label = label && *label ? pin_xstrdup(label) : basename_dup(path);
    r->meta.session_sha = NULL;
    r->meta.created_ms = pin_wall_ms();
    r->meta.last_active_ms = 0;
    records_push(s, r);

    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/workspaces/%s", s->root, r->meta.id);
    ensure_dir_recursive(dir, 0700);

    persist_meta(s, &r->meta);
    persist_catalog(s);

    if (out_id)
        *out_id = pin_xstrdup(r->meta.id);
    pthread_rwlock_unlock(&s->rw);
    return true;
}

static void rmdir_recursive(const char *path)
{
    /* Best-effort: only used for archived dirs we created. */
    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", path);
    /* Defang any quote in path. We control inputs so there should be none. */
    if (strchr(path, '\''))
        return;
    int rc = system(rm);
    (void)rc;
}

bool pin_workspace_store_delete(pin_workspace_store *s, const char *id, char **out_err)
{
    if (out_err)
        *out_err = NULL;
    if (!s || !id) {
        if (out_err)
            *out_err = pin_xstrdup("id required");
        return false;
    }
    pthread_rwlock_wrlock(&s->rw);
    if (s->active_id && !strcmp(s->active_id, id)) {
        pthread_rwlock_unlock(&s->rw);
        if (out_err)
            *out_err = pin_xstrdup("workspace is active; deactivate first");
        return false;
    }
    size_t hit = (size_t)-1;
    for (size_t i = 0; i < s->count; i++) {
        if (s->records[i]->meta.id && !strcmp(s->records[i]->meta.id, id)) {
            hit = i;
            break;
        }
    }
    if (hit == (size_t)-1) {
        pthread_rwlock_unlock(&s->rw);
        if (out_err)
            *out_err = pin_xstrdup("unknown workspace id");
        return false;
    }
    ws_rec *r = s->records[hit];
    /* Archive the workspace dir under _trash/<id>. */
    char src[4096], trash_root[4096], dst[4096];
    snprintf(src, sizeof(src), "%s/workspaces/%s", s->root, r->meta.id);
    snprintf(trash_root, sizeof(trash_root), "%s/_trash", s->root);
    ensure_dir(trash_root, 0700);
    snprintf(dst, sizeof(dst), "%s/%s.%llu", trash_root, r->meta.id,
             (unsigned long long)pin_wall_ms());
    if (rename(src, dst) != 0) {
        rmdir_recursive(src);
    }
    rec_free(r);
    /* Compact array. */
    for (size_t i = hit; i + 1 < s->count; i++)
        s->records[i] = s->records[i + 1];
    s->count--;
    persist_catalog(s);
    pthread_rwlock_unlock(&s->rw);
    return true;
}

bool pin_workspace_store_set_active(pin_workspace_store *s, const char *id)
{
    if (!s)
        return false;
    pthread_rwlock_wrlock(&s->rw);
    if (id) {
        ws_rec *r = find_rec_locked(s, id);
        if (!r) {
            pthread_rwlock_unlock(&s->rw);
            return false;
        }
        r->meta.last_active_ms = pin_wall_ms();
        persist_meta(s, &r->meta);
    }
    free(s->active_id);
    s->active_id = id ? pin_xstrdup(id) : NULL;
    /* Best-effort active_id file (single line). */
    if (s->active_id) {
        atomic_write(s->active_file, s->active_id, strlen(s->active_id));
    } else {
        unlink(s->active_file);
    }
    persist_catalog(s);
    pthread_rwlock_unlock(&s->rw);
    return true;
}

char *pin_workspace_store_get_active(pin_workspace_store *s)
{
    if (!s)
        return NULL;
    pthread_rwlock_rdlock(&s->rw);
    char *out = s->active_id ? pin_xstrdup(s->active_id) : NULL;
    pthread_rwlock_unlock(&s->rw);
    return out;
}

bool pin_workspace_store_set_session_sha(pin_workspace_store *s, const char *id, const char *sha)
{
    if (!s || !id)
        return false;
    pthread_rwlock_wrlock(&s->rw);
    ws_rec *r = find_rec_locked(s, id);
    if (!r) {
        pthread_rwlock_unlock(&s->rw);
        return false;
    }
    free(r->meta.session_sha);
    r->meta.session_sha = (sha && *sha) ? pin_xstrdup(sha) : NULL;
    persist_meta(s, &r->meta);
    persist_catalog(s);
    pthread_rwlock_unlock(&s->rw);
    return true;
}

bool pin_workspace_store_reset(pin_workspace_store *s, const char *id, char **out_err)
{
    if (out_err)
        *out_err = NULL;
    if (!s || !id) {
        if (out_err)
            *out_err = pin_xstrdup("id required");
        return false;
    }
    pthread_rwlock_wrlock(&s->rw);
    ws_rec *r = find_rec_locked(s, id);
    if (!r) {
        pthread_rwlock_unlock(&s->rw);
        if (out_err)
            *out_err = pin_xstrdup("unknown workspace id");
        return false;
    }
    /* Close + truncate the events.log. The supervisor must respawn the
     * agent for this workspace; we don't touch the agent here. */
    if (r->log) {
        pin_event_log_close(r->log);
        r->log = NULL;
    }
    char log_path[4096];
    snprintf(log_path, sizeof(log_path), "%s/workspaces/%s/events.log", s->root, r->meta.id);
    int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd >= 0)
        close(fd);
    free(r->meta.session_sha);
    r->meta.session_sha = NULL;
    persist_meta(s, &r->meta);
    persist_catalog(s);
    pthread_rwlock_unlock(&s->rw);
    return true;
}

static pin_event_log *open_event_log_locked(pin_workspace_store *s, ws_rec *r)
{
    if (r->log)
        return r->log;
    char log_path[4096];
    snprintf(log_path, sizeof(log_path), "%s/workspaces/%s/events.log", s->root, r->meta.id);
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/workspaces/%s", s->root, r->meta.id);
    ensure_dir_recursive(dir, 0700);
    r->log = pin_event_log_open(log_path, s->per_ws_ring_cap);
    return r->log;
}

pin_event_log *pin_workspace_store_event_log(pin_workspace_store *s, const char *id)
{
    if (!s || !id)
        return NULL;
    pthread_rwlock_rdlock(&s->rw);
    ws_rec *r = find_rec_locked(s, id);
    if (!r) {
        pthread_rwlock_unlock(&s->rw);
        return NULL;
    }
    if (r->log) {
        pin_event_log *out = r->log;
        pthread_rwlock_unlock(&s->rw);
        return out;
    }
    pthread_rwlock_unlock(&s->rw);

    pthread_rwlock_wrlock(&s->rw);
    r = find_rec_locked(s, id);
    if (!r) {
        pthread_rwlock_unlock(&s->rw);
        return NULL;
    }
    pin_event_log *out = open_event_log_locked(s, r);
    pthread_rwlock_unlock(&s->rw);
    return out;
}

bool pin_workspace_store_ws_dir(pin_workspace_store *s, const char *id, char *buf, size_t cap)
{
    if (!s || !id || !buf)
        return false;
    int n = snprintf(buf, cap, "%s/workspaces/%s", s->root, id);
    return n > 0 && (size_t)n < cap;
}
