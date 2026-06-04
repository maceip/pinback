#include "../src/util.h"
#include "../src/workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int run_workspace_tests(void);

static int fails = 0;
#define EXPECT(c, m) do { \
    if (c) printf("ok  - %s\n", m); \
    else { printf("not ok - %s (%s:%d)\n", m, __FILE__, __LINE__); fails++; } \
} while (0)

static char *unique_root(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "/tmp/pinback-ws-%d-%lld",
             (int)getpid(), (long long)pin_monotonic_ms());
    char rm[128]; snprintf(rm, sizeof(rm), "rm -rf %s", buf);
    int rc = system(rm); (void)rc;
    return pin_xstrdup(buf);
}

static char *make_dir_under(const char *root, const char *sub) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", root, sub);
    char rm[8192]; snprintf(rm, sizeof(rm), "mkdir -p %s", path);
    int rc = system(rm); (void)rc;
    return pin_xstrdup(path);
}

static bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void test_create_list_get(void) {
    char *root = unique_root();
    pin_workspace_store *s = pin_workspace_store_open(root, 16);
    EXPECT(s != NULL, "store opens with fresh root");

    char *dir_a = make_dir_under(root, "wsA");
    char *dir_b = make_dir_under(root, "wsB");
    char *id_a = NULL;
    char *err = NULL;
    bool ok = pin_workspace_store_create(s, dir_a, "alpha", &id_a, &err);
    EXPECT(ok && id_a, "create workspace A");
    EXPECT(err == NULL, "no error on create A");
    char *id_b = NULL;
    ok = pin_workspace_store_create(s, dir_b, NULL, &id_b, &err);
    EXPECT(ok && id_b, "create workspace B");

    /* Reject duplicate. */
    char *dup_id = NULL;
    char *dup_err = NULL;
    ok = pin_workspace_store_create(s, dir_a, NULL, &dup_id, &dup_err);
    EXPECT(!ok && dup_err, "duplicate path rejected");
    free(dup_err);

    /* Reject relative. */
    char *rel_err = NULL;
    ok = pin_workspace_store_create(s, "relative/path", NULL, NULL, &rel_err);
    EXPECT(!ok && rel_err, "relative path rejected");
    free(rel_err);

    /* List. */
    size_t n = 0;
    pin_workspace_meta *list = pin_workspace_store_list(s, &n);
    EXPECT(n == 2, "list count == 2");
    pin_workspace_meta_array_free(list, n);

    /* Get. */
    pin_workspace_meta got;
    EXPECT(pin_workspace_store_get(s, id_a, &got), "get(id_a)");
    EXPECT(strcmp(got.path, dir_a) == 0, "get returns correct path");
    EXPECT(strcmp(got.label, "alpha") == 0, "get returns label");
    pin_workspace_meta_free(&got);

    pin_workspace_store_close(s);

    /* Reopen and verify persistence. */
    pin_workspace_store *s2 = pin_workspace_store_open(root, 16);
    list = pin_workspace_store_list(s2, &n);
    EXPECT(n == 2, "persisted list count == 2");
    pin_workspace_meta_array_free(list, n);
    pin_workspace_store_close(s2);

    free(id_a); free(id_b); free(dir_a); free(dir_b);
    char rm[8192]; snprintf(rm, sizeof(rm), "rm -rf %s", root);
    int rc = system(rm); (void)rc;
    free(root);
}

static void test_active_session_sha_reset_delete(void) {
    char *root = unique_root();
    pin_workspace_store *s = pin_workspace_store_open(root, 16);
    char *dir_a = make_dir_under(root, "wsA");
    char *id_a = NULL;
    pin_workspace_store_create(s, dir_a, NULL, &id_a, NULL);

    /* set_active. */
    EXPECT(pin_workspace_store_set_active(s, id_a), "set_active");
    char *active = pin_workspace_store_get_active(s);
    EXPECT(active && strcmp(active, id_a) == 0, "get_active matches");
    free(active);

    /* session_sha persists. */
    EXPECT(pin_workspace_store_set_session_sha(s, id_a, "deadbeef"),
           "set_session_sha");
    pin_workspace_meta got;
    pin_workspace_store_get(s, id_a, &got);
    EXPECT(got.session_sha && strcmp(got.session_sha, "deadbeef") == 0,
           "session_sha stored");
    pin_workspace_meta_free(&got);

    /* event_log get-or-open. */
    pin_event_log *log = pin_workspace_store_event_log(s, id_a);
    EXPECT(log != NULL, "event_log opens");
    pin_event_log_append(log, "user", "{\"text\":\"hi\"}", 13);
    char log_path[4096];
    snprintf(log_path, sizeof(log_path), "%s/workspaces/%s/events.log", root, id_a);
    EXPECT(path_exists(log_path), "events.log exists on disk");

    /* reset clears session_sha + truncates events.log. */
    char *err = NULL;
    EXPECT(pin_workspace_store_reset(s, id_a, &err), "reset");
    pin_workspace_meta got2;
    pin_workspace_store_get(s, id_a, &got2);
    EXPECT(got2.session_sha == NULL, "reset cleared session_sha");
    pin_workspace_meta_free(&got2);
    /* After reset, the log handle is closed. Re-fetch. */
    pin_event_log *log2 = pin_workspace_store_event_log(s, id_a);
    pin_event_log_status st;
    pin_event_log_status_get(log2, &st);
    EXPECT(st.newest_seq == 0, "events.log newest_seq reset to 0");

    /* delete fails when active. */
    char *del_err = NULL;
    EXPECT(!pin_workspace_store_delete(s, id_a, &del_err),
           "delete rejected while active");
    free(del_err);

    pin_workspace_store_set_active(s, NULL);
    EXPECT(pin_workspace_store_delete(s, id_a, NULL), "delete after deactivate");

    size_t n = 0;
    pin_workspace_meta *list = pin_workspace_store_list(s, &n);
    EXPECT(n == 0, "list empty after delete");
    pin_workspace_meta_array_free(list, n);

    pin_workspace_store_close(s);
    free(id_a); free(dir_a);
    char rm[8192]; snprintf(rm, sizeof(rm), "rm -rf %s", root);
    int rc = system(rm); (void)rc;
    free(root);
}

int run_workspace_tests(void) {
    fails = 0;
    test_create_list_get();
    test_active_session_sha_reset_delete();
    return fails ? 1 : 0;
}
