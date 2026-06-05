/* Test runner: tiny TAP-ish harness for the modules above. Each
 * test_*.c file contributes a `run_*` entry point listed below. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int run_util_tests(void);
extern int run_event_log_tests(void);
extern int run_http_tests(void);
extern int run_workspace_tests(void);
extern int run_agent_tests(void);
extern int run_tracestream_tests(void);

typedef int (*suite_fn)(void);

typedef struct {
    const char *name;
    suite_fn    fn;
} suite;

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    suite suites[] = {
        {"util",      run_util_tests},
        {"event_log", run_event_log_tests},
        {"http",      run_http_tests},
        {"workspace", run_workspace_tests},
        {"agent",     run_agent_tests},
        {"tracestream", run_tracestream_tests},
    };
    size_t n = sizeof(suites) / sizeof(suites[0]);
    int failed = 0;
    int total = 0;
    for (size_t i = 0; i < n; i++) {
        if (argc > 1 && strcmp(argv[1], suites[i].name) != 0) continue;
        printf("# suite %s\n", suites[i].name);
        int r = suites[i].fn();
        if (r != 0) failed++;
        total++;
    }
    if (failed == 0) {
        printf("OK  all %d suites passed\n", total);
        return 0;
    }
    printf("FAIL %d/%d suite(s) failed\n", failed, total);
    return 1;
}
