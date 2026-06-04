// Pinback macOS shell — smallest-binary variant: a native AppKit window hosting
// WKWebView, in pure Objective-C (no Swift/SwiftUI runtime). The window chrome
// and the WKWebView engine are the OS's, so this paints identically to a SwiftUI
// shell while producing a much smaller binary.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        // Required for an un-bundled binary to show a window + menubar.
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

        const char *env = getenv("PINBACK_URL");
        NSString *url = env ? [NSString stringWithUTF8String:env]
                            : @"http://127.0.0.1:18192";
        [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];

        [win center];
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}
