/* fake-ds4-agent: deterministic stand-in for ds4-agent --non-interactive.
 *
 * This mirrors the REAL ds4-agent's verified non-interactive contract
 * (see docs/architecture/transport-findings.md), not the v0 assumptions:
 *
 *   - Honors --non-interactive, --chdir <dir>, --model <path> (no-op).
 *   - On boot and after every turn, prints "+DWARFSTAR_WAITING" on
 *     STDERR. pinback uses that marker (not any stdout sentinel) as the
 *     turn-end / idle signal.
 *   - stdout carries clean prose. Tool actions are rendered the way the
 *     real agent renders them: a line beginning with the wrench glyph
 *     "U+1F6E0 U+FE0F" -- "\xF0\x9F\x9B\xA0\xEF\xB8\x8F" -- followed by
 *     "<tool> <params>". The real agent does NOT emit raw DSML in any
 *     mode, so neither do we.
 *   - Slash commands are NOT special: in --non-interactive the real
 *     agent feeds them to the model as prompts. So "/save" here just
 *     produces an ordinary reply; there is no session SHA. Resume is
 *     pinback's job (transcript re-prefill), not the agent's.
 *   - /quit / /exit still exit so tests can stop us deterministically.
 *
 * Output is line-buffered. */

#include "../src/util.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Wrench + emoji variation selector: how ds4-agent prefixes a tool action. */
#define TOOL_GLYPH "\xF0\x9F\x9B\xA0\xEF\xB8\x8F"

static const char *g_workdir = "";

static void out(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

/* The idle/turn-end marker lives on stderr, exactly like the real agent. */
static void mark_waiting(void)
{
    fputs("+DWARFSTAR_WAITING\n", stderr);
    fflush(stderr);
}

/* A deterministic turn: a line of prose, a rendered tool action, a
 * closing line of prose. Shapes match what the classifier must handle. */
static void emit_turn(const char *line)
{
    char buf[8192];
    snprintf(buf, sizeof(buf), "echo: %s\n\n", line);
    out(buf);
    out(TOOL_GLYPH " bash  command=echo hi\n");
    out("hi\n\n");
    out("done.\n\n");
    mark_waiting();
}

static void on_sig(int sig)
{
    (void)sig; /* abort current turn. */
}

int main(int argc, char **argv)
{
    bool non_interactive = false;
    const char *model = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--non-interactive"))
            non_interactive = true;
        else if (!strcmp(a, "--chdir") && i + 1 < argc)
            g_workdir = argv[++i];
        else if (!strcmp(a, "--model") && i + 1 < argc)
            model = argv[++i];
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("fake-ds4-agent test stub\n");
            return 0;
        } else {
            fprintf(stderr, "fake-ds4-agent: unknown arg %s\n", a);
        }
    }
    (void)non_interactive;
    (void)model;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_sig);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "fake-ds4-agent ready (chdir=%s)\n", g_workdir[0] ? g_workdir : "(none)");
    /* Initial readiness, like the real agent announcing it is loaded. */
    mark_waiting();

    /* Batch stdin the way the real agent does: accumulate bytes until
     * input has been quiet for ~120ms, then treat the whole buffer
     * (possibly multi-line, e.g. a re-prefill transcript) as one prompt. */
    pin_buf acc;
    pin_buf_init(&acc);
    for (;;) {
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
        int pr = poll(&pfd, 1, acc.len ? 120 : -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) { /* quiet: flush one prompt */
            char *p = acc.ptr;
            size_t len = acc.len;
            while (len && (p[len - 1] == '\n' || p[len - 1] == ' '))
                len--;
            char *prompt = pin_xstrndup(p, len);
            if (!strcmp(prompt, "/quit") || !strcmp(prompt, "/exit")) {
                free(prompt);
                exit(0);
            }
            if (prompt[0])
                emit_turn(prompt);
            free(prompt);
            pin_buf_clear(&acc);
            continue;
        }
        char rb[8192];
        ssize_t k = read(STDIN_FILENO, rb, sizeof(rb));
        if (k <= 0)
            break; /* EOF / error: stdin closed */
        pin_buf_append(&acc, rb, (size_t)k);
    }
    pin_buf_free(&acc);
    return 0;
}
