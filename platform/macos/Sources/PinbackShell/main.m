// Pinback macOS shell — smallest-binary variant: a native AppKit window hosting
// WKWebView, in pure Objective-C (no Swift/SwiftUI runtime). The window chrome
// and the WKWebView engine are the OS's, so this paints identically to a SwiftUI
// shell while producing a much smaller binary.
//
// Embedding model (how the shell "loads pinback"):
//   - If PINBACK_URL is set, load it verbatim and spawn nothing (dev/remote).
//   - Otherwise the shell IS the launcher: it spawns the bundled
//     `pinback-server` child on 127.0.0.1:8088, waits for /healthz, then loads
//     it, and terminates the server when the app quits.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <crt_externs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t g_server_pid = -1;

static int health_ok(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(8088);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return 0;
    }
    const char *req = "GET /healthz HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) {
        close(fd);
        return 0;
    }
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 12 && memcmp(buf, "HTTP/", 5) == 0 && strstr(buf, " 200") != NULL;
}

static void stop_server(void) {
    if (g_server_pid > 0) {
        kill(g_server_pid, SIGTERM);
        waitpid(g_server_pid, NULL, 0);
        g_server_pid = -1;
    }
}

static NSString *server_binary_path(void) {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableArray<NSString *> *candidates = [NSMutableArray array];
    NSString *exe = [[NSBundle mainBundle] executablePath];
    if (exe) {
        NSString *dir = [exe stringByDeletingLastPathComponent];
        [candidates addObject:[dir stringByAppendingPathComponent:@"pinback-server"]];
    }
    NSString *res = [[NSBundle mainBundle] resourcePath];
    if (res) {
        [candidates addObject:[res stringByAppendingPathComponent:@"pinback-server"]];
    }
    [candidates addObject:@"/opt/homebrew/bin/pinback-server"];
    [candidates addObject:@"/usr/local/bin/pinback-server"];
    const char *override = getenv("PINBACK_SERVER_BIN");
    if (override && *override) {
        [candidates insertObject:[NSString stringWithUTF8String:override] atIndex:0];
    }
    for (NSString *path in candidates) {
        if ([fm isExecutableFileAtPath:path]) return path;
    }
    return @"pinback-server";
}

static NSString *spawn_server(void) {
    NSString *bin = server_binary_path();
    char *argv[] = {
        (char *)bin.UTF8String,
        "--bind", "127.0.0.1:8088",
        "--quiet",
        NULL
    };
    pid_t pid = 0;
    if (posix_spawn(&pid, bin.UTF8String, NULL, NULL, argv, *_NSGetEnviron()) != 0) {
        return @"http://127.0.0.1:8088";
    }
    g_server_pid = pid;
    for (int i = 0; i < 150 && !health_ok(); i++)
        usleep(200 * 1000);
    return @"http://127.0.0.1:8088";
}

static NSString *start_url(void) {
    const char *env = getenv("PINBACK_URL");
    if (env && *env) return [NSString stringWithUTF8String:env];
    return spawn_server();
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSRect frame = NSMakeRect(0, 0, 1280, 800);
        NSWindow *win = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        win.title = @"Pinback";

        WKWebView *web = [[WKWebView alloc] initWithFrame:frame];
        web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        win.contentView = web;

        NSString *url = start_url();
        [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];

        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationWillTerminateNotification
                        object:nil
                         queue:nil
                    usingBlock:^(__unused NSNotification *n) { stop_server(); }];

        [win center];
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    stop_server();
    return 0;
}
