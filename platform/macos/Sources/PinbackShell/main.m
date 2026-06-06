// Pinback macOS shell — smallest-binary variant: a native AppKit app in pure
// Objective-C (no Swift/SwiftUI runtime) hosting WKWebView. The window chrome and
// the WKWebView engine are the OS's, so this paints identically to a SwiftUI shell
// while producing a much smaller binary.
//
// Embedding model (how the shell "loads pinback"):
//   - If PINBACK_URL is set, load it verbatim and spawn nothing (dev/remote).
//   - Otherwise the shell IS the launcher: it spawns the bundled `pinback-server`
//     child on 127.0.0.1:8088, waits for /healthz, then loads it, and terminates
//     the server when the app quits.
//
// Sugar (macOS 26 "Liquid Glass", AppKit edition):
//   - NSSplitViewController gives a native workspace sidebar + WKWebView detail +
//     an inspector. The sidebar and the toolbar adopt the system Liquid Glass
//     automatically when built/run on macOS 26 — so we deliberately do NOT
//     hand-roll glass (which is the only case that would need an
//     NSGlassEffectContainerView). This is the AppKit analogue of SwiftUI's
//     NavigationSplitView + .inspector(isPresented:).
//   - The toolbar is divided into sections with a flexible space and a fixed
//     space (NSToolbarFlexibleSpaceItemIdentifier / NSToolbarSpaceItemIdentifier)
//     — the AppKit equivalent of ToolbarSpacer(.flexible) / ToolbarSpacer(.fixed).
//   - The web content is wrapped in NSBackgroundExtensionView (macOS 26) so it
//     bleeds edge-to-edge under the sidebar/titlebar glass — the AppKit analogue
//     of .backgroundExtensionEffect(). Loaded dynamically + @available-guarded so
//     the shell still compiles on older SDKs and runs back to macOS 13.
//   - A WKScriptMessageHandler ("pinback") mirrors the cockpit's workspace list
//     into the native sidebar/inspector; selecting a row drives the page via
//     window.pinback.selectWorkspace(). Light/dark is automatic (WKWebView and
//     the window both follow the system appearance).
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "pinback_url.h"

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
static BOOL g_in_setup = NO;
static char g_cockpit_url[PINBACK_URL_MAX];

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
    char url[PINBACK_URL_MAX];
    pinback_url_resolve(url, sizeof url, pinback_url_default());
    for (int i = 0; i < 150 && !pinback_health_ok(url); i++)
        usleep(200 * 1000);
    return [NSString stringWithUTF8String:url];
}

static NSString *pinback_js_escape(NSString *s) {
    s = [s stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    return [s stringByReplacingOccurrencesOfString:@"'" withString:@"\\'"];
}

static void load_cockpit(WKWebView *web, NSString *url) {
    g_in_setup = NO;
    strncpy(g_cockpit_url, url.UTF8String, sizeof g_cockpit_url - 1);
    [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
}

static void begin_session(WKWebView *web) {
    const char *env = getenv("PINBACK_URL");
    if (env && *env) {
        load_cockpit(web, [NSString stringWithUTF8String:env]);
        return;
    }
    char url[PINBACK_URL_MAX];
    pinback_url_resolve(url, sizeof url, pinback_url_default());
    NSString *nsurl = [NSString stringWithUTF8String:url];
    if (pinback_health_ok(url)) {
        load_cockpit(web, nsurl);
        return;
    }
    (void)spawn_server();
    pinback_url_resolve(url, sizeof url, pinback_url_default());
    nsurl = [NSString stringWithUTF8String:url];
    if (pinback_health_ok(url)) {
        load_cockpit(web, nsurl);
        return;
    }
    char setup_uri[PINBACK_URL_MAX + 16];
    if (pinback_setup_file_uri(setup_uri, sizeof setup_uri) == 0) {
        g_in_setup = YES;
        NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:setup_uri]];
        [web loadRequest:[NSURLRequest requestWithURL:u]];
        if (nsurl.length) {
            NSString *js = [NSString stringWithFormat:
                @"document.getElementById('url').value='%@';",
                pinback_js_escape(nsurl)];
            [web evaluateJavaScript:js completionHandler:nil];
        }
    } else {
        load_cockpit(web, nsurl);
    }
}

// ---------------------------------------------------------------------------
// Toolbar item identifiers.
// ---------------------------------------------------------------------------
static NSString *const kReloadItem = @"pinback.reload";
static NSString *const kNewWorkspaceItem = @"pinback.newWorkspace";
static NSString *const kShareItem = @"pinback.share";
static NSString *const kInspectorItem = @"pinback.inspector";
static NSString *const kServerItem = @"pinback.server";

// ===========================================================================
// Sidebar: native workspace switcher, fed by the JS bridge.
// ===========================================================================
@interface WorkspaceRow : NSObject
@property(nonatomic, copy) NSString *wid;
@property(nonatomic, copy) NSString *label;
@property(nonatomic, copy) NSString *path;
@end
@implementation WorkspaceRow
@end

@protocol SidebarDelegate <NSObject>
- (void)sidebarDidActivateWorkspace:(NSString *)wid;
@end

@interface SidebarController : NSViewController <NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic, weak) id<SidebarDelegate> delegate;
@property(nonatomic, strong) NSMutableArray<WorkspaceRow *> *rows;
@property(nonatomic, copy) NSString *activeId;
@property(nonatomic, strong) NSTableView *table;
- (void)setWorkspaces:(NSArray<WorkspaceRow *> *)rows active:(NSString *)activeId;
@end

@implementation SidebarController

- (void)loadView {
    self.rows = [NSMutableArray array];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 240, 600)];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;            // let the sidebar glass show through

    NSTableView *table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"ws"];
    col.title = @"Workspaces";
    [table addTableColumn:col];
    table.headerView = nil;
    table.dataSource = self;
    table.delegate = self;
    table.style = NSTableViewStyleSourceList;   // sidebar / source-list styling
    table.backgroundColor = NSColor.clearColor;
    table.rowHeight = 40;
    table.target = self;
    table.action = @selector(rowClicked:);
    scroll.documentView = table;
    self.table = table;
    self.view = scroll;
}

- (void)setWorkspaces:(NSArray<WorkspaceRow *> *)rows active:(NSString *)activeId {
    [self.rows setArray:rows];
    self.activeId = activeId;
    [self.table reloadData];
    NSInteger sel = NSNotFound;
    for (NSUInteger i = 0; i < rows.count; i++) {
        if ([rows[i].wid isEqualToString:activeId ?: @""]) { sel = (NSInteger)i; break; }
    }
    if (sel != NSNotFound)
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)sel]
                byExtendingSelection:NO];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv { return (NSInteger)self.rows.count; }

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col row:(NSInteger)r {
    WorkspaceRow *ws = self.rows[(NSUInteger)r];
    NSTableCellView *cell = [tv makeViewWithIdentifier:@"cell" owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, 240, 40)];
        cell.identifier = @"cell";
        NSTextField *title = [NSTextField labelWithString:@""];
        title.translatesAutoresizingMaskIntoConstraints = NO;
        title.lineBreakMode = NSLineBreakByTruncatingTail;
        [cell addSubview:title];
        cell.textField = title;
        NSTextField *sub = [NSTextField labelWithString:@""];
        sub.translatesAutoresizingMaskIntoConstraints = NO;
        sub.font = [NSFont systemFontOfSize:10];
        sub.textColor = NSColor.secondaryLabelColor;
        sub.lineBreakMode = NSLineBreakByTruncatingHead;
        sub.tag = 99;
        [cell addSubview:sub];
        [NSLayoutConstraint activateConstraints:@[
            [title.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:8],
            [title.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-8],
            [title.topAnchor constraintEqualToAnchor:cell.topAnchor constant:5],
            [sub.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:8],
            [sub.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-8],
            [sub.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:1],
        ]];
    }
    cell.textField.stringValue = ws.label.length ? ws.label : ws.wid;
    NSTextField *sub = [cell viewWithTag:99];
    sub.stringValue = ws.path ?: @"";
    return cell;
}

- (void)rowClicked:(id)sender {
    NSInteger r = self.table.clickedRow;
    if (r < 0 || r >= (NSInteger)self.rows.count) return;
    WorkspaceRow *ws = self.rows[(NSUInteger)r];
    if (![ws.wid isEqualToString:self.activeId ?: @""])
        [self.delegate sidebarDidActivateWorkspace:ws.wid];
}

@end

// ===========================================================================
// Inspector: details about the active workspace.
// ===========================================================================
@interface InspectorController : NSViewController
@property(nonatomic, strong) NSTextField *labelField;
@property(nonatomic, strong) NSTextField *pathField;
@property(nonatomic, strong) NSTextField *countField;
- (void)showLabel:(NSString *)label path:(NSString *)path count:(NSUInteger)count;
@end

@implementation InspectorController
- (void)loadView {
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 260, 600)];
    NSStackView *stack = [[NSStackView alloc] init];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 6;
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField *head = [NSTextField labelWithString:@"Active workspace"];
    head.font = [NSFont boldSystemFontOfSize:13];
    self.labelField = [NSTextField labelWithString:@"—"];
    self.labelField.font = [NSFont systemFontOfSize:12];
    self.pathField = [NSTextField wrappingLabelWithString:@""];
    self.pathField.font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
    self.pathField.textColor = NSColor.secondaryLabelColor;
    self.countField = [NSTextField labelWithString:@""];
    self.countField.font = [NSFont systemFontOfSize:11];
    self.countField.textColor = NSColor.secondaryLabelColor;

    [stack addArrangedSubview:head];
    [stack addArrangedSubview:self.labelField];
    [stack addArrangedSubview:self.pathField];
    [stack addArrangedSubview:self.countField];
    [v addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:14],
        [stack.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-14],
        [stack.topAnchor constraintEqualToAnchor:v.topAnchor constant:14],
    ]];
    self.view = v;
}

- (void)showLabel:(NSString *)label path:(NSString *)path count:(NSUInteger)count {
    self.labelField.stringValue = label.length ? label : @"—";
    self.pathField.stringValue = path ?: @"";
    self.countField.stringValue = [NSString stringWithFormat:@"%lu workspace%@",
                                   (unsigned long)count, count == 1 ? @"" : @"s"];
}
@end

// ===========================================================================
// Detail: the WKWebView, optionally wrapped in NSBackgroundExtensionView.
// ===========================================================================
@interface DetailController : NSViewController
@property(nonatomic, strong) WKWebView *web;
@end

@implementation DetailController
- (instancetype)initWithWebView:(WKWebView *)web {
    if ((self = [super init])) { _web = web; }
    return self;
}
- (void)loadView {
    self.web.translatesAutoresizingMaskIntoConstraints = YES;
    self.web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    // macOS 26: let the web content extend edge-to-edge under the chrome glass.
    // Referenced dynamically so this still builds on pre-26 SDKs and runs on 13+.
    if (@available(macOS 26.0, *)) {
        Class bevClass = NSClassFromString(@"NSBackgroundExtensionView");
        if (bevClass) {
            NSView *bev = [[bevClass alloc] initWithFrame:self.web.frame];
            bev.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            [bev setValue:self.web forKey:@"contentView"];   // NSBackgroundExtensionView.contentView
            self.view = bev;
            return;
        }
    }
    self.view = self.web;
}
@end

// ===========================================================================
// Shell: the split view controller wiring sidebar + detail + inspector.
// ===========================================================================
@interface ShellController : NSSplitViewController <SidebarDelegate>
@property(nonatomic, strong) SidebarController *sidebar;
@property(nonatomic, strong) DetailController *detail;
@property(nonatomic, strong) InspectorController *inspector;
@property(nonatomic, strong) NSSplitViewItem *inspectorItem;
@property(nonatomic, strong) WKWebView *web;
@end

@implementation ShellController

- (instancetype)initWithWebView:(WKWebView *)web {
    if ((self = [super init])) {
        _web = web;
        _sidebar = [[SidebarController alloc] init];
        _sidebar.delegate = self;
        _detail = [[DetailController alloc] initWithWebView:web];
        _inspector = [[InspectorController alloc] init];

        NSSplitViewItem *sideItem = [NSSplitViewItem sidebarWithViewController:_sidebar];
        sideItem.minimumThickness = 200;
        sideItem.maximumThickness = 360;
        [self addSplitViewItem:sideItem];

        [self addSplitViewItem:[NSSplitViewItem splitViewItemWithViewController:_detail]];

        if (@available(macOS 14.0, *)) {
            _inspectorItem = [NSSplitViewItem inspectorWithViewController:_inspector];
            _inspectorItem.collapsed = YES;          // hidden until the user opens it
            [self addSplitViewItem:_inspectorItem];
        }
    }
    return self;
}

- (void)sidebarDidActivateWorkspace:(NSString *)wid {
    NSString *js = [NSString stringWithFormat:
        @"window.pinback&&window.pinback.selectWorkspace('%@')", [self jsEscape:wid]];
    [self.web evaluateJavaScript:js completionHandler:nil];
}

- (NSString *)jsEscape:(NSString *)s {
    s = [s stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    s = [s stringByReplacingOccurrencesOfString:@"'" withString:@"\\'"];
    return s;
}

// Toolbar actions ----------------------------------------------------------
- (void)reload:(id)sender { [self.web reload]; }

- (void)newWorkspace:(id)sender {
    // Open the cockpit's own "add workspace" tray (the page owns the form).
    [self.web evaluateJavaScript:@"document.getElementById('ws-button')?.click()"
               completionHandler:nil];
}

- (void)share:(id)sender {
    NSURL *url = self.web.URL;
    if (!url) return;
    NSSharingServicePicker *picker =
        [[NSSharingServicePicker alloc] initWithItems:@[url]];
    NSView *anchor = sender;
    if ([anchor isKindOfClass:[NSView class]])
        [picker showRelativeToRect:anchor.bounds ofView:anchor preferredEdge:NSMinYEdge];
}

- (void)toggleInspector:(id)sender {
    if (self.inspectorItem)
        self.inspectorItem.animator.collapsed = !self.inspectorItem.collapsed;
}

- (void)openServerSettings:(id)sender {
    (void)sender;
    char setup_uri[PINBACK_URL_MAX + 16];
    if (pinback_setup_file_uri(setup_uri, sizeof setup_uri) != 0) return;
    g_in_setup = YES;
    NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:setup_uri]];
    [self.web loadRequest:[NSURLRequest requestWithURL:u]];
    if (g_cockpit_url[0]) {
        NSString *prefill = [NSString stringWithUTF8String:g_cockpit_url];
        NSString *js = [NSString stringWithFormat:
            @"document.getElementById('url').value='%@';", pinback_js_escape(prefill)];
        [self.web evaluateJavaScript:js completionHandler:nil];
    }
}

@end

// ===========================================================================
// App delegate: window, toolbar, server lifecycle, JS bridge.
// ===========================================================================
@interface AppDelegate : NSObject <NSApplicationDelegate, NSToolbarDelegate, WKScriptMessageHandler>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) ShellController *shell;
@property(nonatomic, strong) WKWebView *web;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
    [cfg.userContentController addScriptMessageHandler:self name:@"pinback"];
    [cfg.userContentController addScriptMessageHandler:self name:@"pinbackSetup"];
    self.web = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 800)
                                  configuration:cfg];

    self.shell = [[ShellController alloc] initWithWebView:self.web];

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1280, 800)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable |
                             NSWindowStyleMaskFullSizeContentView)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = @"Pinback";
    win.titlebarAppearsTransparent = NO;
    win.contentViewController = self.shell;

    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"pinback.toolbar"];
    toolbar.delegate = self;
    toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    win.toolbar = toolbar;
    if (@available(macOS 11.0, *)) win.toolbarStyle = NSWindowToolbarStyleUnified;

    self.window = win;

    begin_session(self.web);

    [win center];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)applicationWillTerminate:(NSNotification *)note { stop_server(); }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }

// web -> native: workspace list / active / canGoBack.
- (void)userContentController:(WKUserContentController *)ucc
      didReceiveScriptMessage:(WKScriptMessage *)message {
    if (![message.body isKindOfClass:[NSDictionary class]]) return;
    NSDictionary *body = message.body;

    if ([message.name isEqualToString:@"pinback"] &&
        [body[@"type"] isEqualToString:@"pinback-host"] &&
        [body[@"action"] isEqualToString:@"openSetup"]) {
        [self.shell openServerSettings:nil];
        return;
    }

    if ([message.name isEqualToString:@"pinbackSetup"] ||
        [body[@"type"] isEqualToString:@"pinback-setup"]) {
        NSString *action = body[@"action"];
        NSString *url = body[@"url"];
        if (![url isKindOfClass:[NSString class]] || !url.length) return;
        if ([action isEqualToString:@"test"]) {
            if (pinback_health_ok(url.UTF8String))
                [self.web evaluateJavaScript:
                    @"document.getElementById('status').textContent='Server is reachable.';"
                    @"document.getElementById('status').className='ok';"
                    completionHandler:nil];
            else
                [self.web evaluateJavaScript:
                    @"document.getElementById('status').textContent='Cannot reach server at that URL.';"
                    @"document.getElementById('status').className='err';"
                    completionHandler:nil];
            return;
        }
        if ([action isEqualToString:@"connect"]) {
            pinback_url_save(url.UTF8String);
            if (pinback_health_ok(url.UTF8String))
                load_cockpit(self.web, url);
            else
                [self.web evaluateJavaScript:
                    @"document.getElementById('status').textContent='Saved, but server still unreachable.';"
                    @"document.getElementById('status').className='err';"
                    completionHandler:nil];
        }
        return;
    }

    if (g_in_setup || ![body[@"type"] isEqual:@"workspaces"]) return;

    NSArray *list = body[@"workspaces"];
    NSString *activeId = body[@"activeId"];
    NSMutableArray<WorkspaceRow *> *rows = [NSMutableArray array];
    NSString *activeLabel = @"", *activePath = @"";
    if ([list isKindOfClass:[NSArray class]]) {
        for (NSDictionary *w in list) {
            if (![w isKindOfClass:[NSDictionary class]]) continue;
            WorkspaceRow *row = [[WorkspaceRow alloc] init];
            row.wid = [w[@"id"] isKindOfClass:[NSString class]] ? w[@"id"] : @"";
            row.label = [w[@"label"] isKindOfClass:[NSString class]] ? w[@"label"] : @"";
            row.path = [w[@"path"] isKindOfClass:[NSString class]] ? w[@"path"] : @"";
            [rows addObject:row];
            if ([row.wid isEqualToString:activeId ?: @""]) {
                activeLabel = row.label;
                activePath = row.path;
            }
        }
    }
    [self.shell.sidebar setWorkspaces:rows active:activeId];
    [self.shell.inspector showLabel:activeLabel path:activePath count:rows.count];
}

// Toolbar (NSToolbarDelegate) ----------------------------------------------
- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)tb {
    return @[
        NSToolbarToggleSidebarItemIdentifier,
        NSToolbarSidebarTrackingSeparatorItemIdentifier,
        kReloadItem,
        kNewWorkspaceItem,
        NSToolbarFlexibleSpaceItemIdentifier,   // == ToolbarSpacer(.flexible)
        kShareItem,
        NSToolbarSpaceItemIdentifier,           // == ToolbarSpacer(.fixed)
        kServerItem,
        kInspectorItem,
    ];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)tb {
    return [self toolbarDefaultItemIdentifiers:tb];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)tb
     itemForItemIdentifier:(NSToolbarItemIdentifier)id
 willBeInsertedIntoToolbar:(BOOL)flag {
    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:id];
    item.target = self.shell;
    if ([id isEqualToString:kReloadItem]) {
        item.label = @"Reload";
        item.image = [NSImage imageWithSystemSymbolName:@"arrow.clockwise" accessibilityDescription:@"Reload"];
        item.action = @selector(reload:);
    } else if ([id isEqualToString:kNewWorkspaceItem]) {
        item.label = @"Workspaces";
        item.image = [NSImage imageWithSystemSymbolName:@"square.grid.2x2" accessibilityDescription:@"Workspaces"];
        item.action = @selector(newWorkspace:);
    } else if ([id isEqualToString:kShareItem]) {
        item.label = @"Share";
        item.image = [NSImage imageWithSystemSymbolName:@"square.and.arrow.up" accessibilityDescription:@"Share"];
        item.action = @selector(share:);
    } else if ([id isEqualToString:kServerItem]) {
        item.label = @"Server";
        item.image = [NSImage imageWithSystemSymbolName:@"network" accessibilityDescription:@"Server"];
        item.action = @selector(openServerSettings:);
    } else if ([id isEqualToString:kInspectorItem]) {
        item.label = @"Info";
        item.image = [NSImage imageWithSystemSymbolName:@"info.circle" accessibilityDescription:@"Info"];
        item.action = @selector(toggleInspector:);
    }
    return item;
}

@end

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    stop_server();
    return 0;
}
