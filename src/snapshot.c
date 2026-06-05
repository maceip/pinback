#include "snapshot.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Run `git <args...>` in an isolated shadow repo. The child chdir's to
 * work_tree (so `git apply` resolves paths there) and points GIT_DIR at
 * the absolute shadow git dir. Optional small stdin (a patch); stdout is
 * captured into `out` (may be NULL). Returns the exit code, or -1 on a
 * spawn failure. */
static int run_git(const char *git_dir, const char *work_tree, const char *const args[],
                   const char *in, size_t in_len, pin_buf *out)
{
    int inp[2] = {-1, -1}, outp[2] = {-1, -1};
    if (pipe(inp) != 0 || pipe(outp) != 0) {
        if (inp[0] >= 0) {
            close(inp[0]);
            close(inp[1]);
        }
        if (outp[0] >= 0) {
            close(outp[0]);
            close(outp[1]);
        }
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(inp[0]);
        close(inp[1]);
        close(outp[0]);
        close(outp[1]);
        return -1;
    }
    if (pid == 0) {
        if (chdir(work_tree) != 0)
            _exit(126);
        dup2(inp[0], STDIN_FILENO);
        dup2(outp[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(inp[0]);
        close(inp[1]);
        close(outp[0]);
        close(outp[1]);
        setenv("GIT_DIR", git_dir, 1);
        setenv("GIT_WORK_TREE", work_tree, 1);
        setenv("GIT_AUTHOR_NAME", "pinback", 1);
        setenv("GIT_AUTHOR_EMAIL", "pinback@local", 1);
        setenv("GIT_COMMITTER_NAME", "pinback", 1);
        setenv("GIT_COMMITTER_EMAIL", "pinback@local", 1);
        setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
        setenv("GIT_TERMINAL_PROMPT", "0", 1);
        const char *argv[64];
        int n = 0;
        argv[n++] = "git";
        for (int i = 0; args[i] && n < 63; i++)
            argv[n++] = args[i];
        argv[n] = NULL;
        execvp("git", (char *const *)argv);
        _exit(127);
    }
    close(inp[0]);
    close(outp[1]);
    if (in && in_len) {
        ssize_t w = write(inp[1], in, in_len);
        (void)w;
    }
    close(inp[1]);
    char buf[4096];
    ssize_t k;
    while ((k = read(outp[0], buf, sizeof(buf))) > 0)
        if (out)
            pin_buf_append(out, buf, (size_t)k);
    close(outp[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static bool ensure_repo(const char *git_dir, const char *work_tree)
{
    char head[4200];
    snprintf(head, sizeof(head), "%s/HEAD", git_dir);
    struct stat sb;
    if (stat(head, &sb) == 0)
        return true;
    const char *init[] = {"init", "-q", NULL};
    if (run_git(git_dir, work_tree, init, NULL, 0, NULL) != 0)
        return false;
    /* Keep snapshots cheap and avoid recursing into the user's own VCS or
     * heavy build trees. info/exclude is private to the shadow repo. */
    char excl[4200];
    snprintf(excl, sizeof(excl), "%s/info/exclude", git_dir);
    FILE *f = fopen(excl, "w");
    if (f) {
        fputs(".git/\nnode_modules/\n.venv/\nvenv/\ntarget/\ndist/\n"
              "build/\n.DS_Store\n*.o\n*.gguf\n*.bin\n*.kv\n",
              f);
        fclose(f);
    }
    return true;
}

bool pin_snapshot_begin(const char *git_dir, const char *work_tree)
{
    if (!git_dir || !work_tree)
        return false;
    if (!ensure_repo(git_dir, work_tree))
        return false;
    const char *add[] = {"add", "-A", NULL};
    run_git(git_dir, work_tree, add, NULL, 0, NULL);
    const char *commit[] = {"commit", "-q", "--allow-empty", "-m", "turn-start", NULL};
    int rc = run_git(git_dir, work_tree, commit, NULL, 0, NULL);
    return rc == 0;
}

bool pin_snapshot_diff(const char *git_dir, const char *work_tree, pin_buf *out)
{
    if (!git_dir || !work_tree || !out)
        return false;
    char head[4200];
    snprintf(head, sizeof(head), "%s/HEAD", git_dir);
    struct stat sb;
    if (stat(head, &sb) != 0)
        return false; /* no baseline yet */
    const char *add[] = {"add", "-A", NULL};
    run_git(git_dir, work_tree, add, NULL, 0, NULL);
    const char *diff[] = {"diff", "--cached", "--no-color", "HEAD", NULL};
    return run_git(git_dir, work_tree, diff, NULL, 0, out) >= 0;
}

bool pin_snapshot_revert(const char *git_dir, const char *work_tree, const char *patch,
                         size_t patch_len, char *errbuf, size_t errcap)
{
    if (!git_dir || !work_tree || !patch || patch_len == 0) {
        if (errbuf && errcap)
            snprintf(errbuf, errcap, "empty patch");
        return false;
    }
    const char *apply[] = {"apply", "-R", "--whitespace=nowarn", NULL};
    int rc = run_git(git_dir, work_tree, apply, patch, patch_len, NULL);
    if (rc != 0) {
        if (errbuf && errcap)
            snprintf(errbuf, errcap, "git apply -R failed (rc=%d)", rc);
        return false;
    }
    return true;
}
