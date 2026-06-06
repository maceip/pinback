// Pinback Windows shell: a Win32 window hosting a WebView2 control. WebView2 is
// powered by the system-installed Microsoft Edge (Chromium) "Evergreen" runtime,
// so no browser engine is bundled. This build also ships NO WebView2Loader.dll:
// it locates the runtime via the registry and calls its internal environment
// entry point directly (the same technique the webview/webview library uses), so
// the deliverable is a single self-contained ~tens-of-KB exe. WRL (Windows SDK,
// header-only) is kept for the async COM callbacks.
//
// Embedding model (how the shell "loads pinback"):
//   - If PINBACK_URL is set, load it verbatim and spawn nothing (dev/remote).
//   - Otherwise the shell IS the launcher: it spawns the bundled
//     `pinback-server.exe` child on 127.0.0.1:8088, waits for /healthz, then
//     loads it, and terminates the child on window close.
//
// Sugar (Windows 11, 2026):
//   - Mica system backdrop (DWMWA_SYSTEMBACKDROP_TYPE / DWMSBT_MAINWINDOW) and a
//     dark/light caption that follows the OS theme live (DWMWA_USE_IMMERSIVE_DARK
//     _MODE, re-applied on WM_SETTINGCHANGE). All DWM calls are build-gated: they
//     no-op (failed HRESULT, ignored) on Windows 10 / pre-22621.
//   - The WebView2 is given a transparent default background and its
//     PreferredColorScheme follows the OS, so content/scrollbars/dialogs theme
//     correctly and the Mica can show through any transparent page regions.
//   - A native menubar carries app controls: a *Workspaces* menu rebuilt live
//     from the cockpit (it posts its workspace list over chrome.webview), and a
//     *View* menu (Previous workspace / Reload / Theme). Selecting a workspace
//     drives window.pinback.selectWorkspace() back in the page.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

extern "C" {
#include "pinback_url.h"
}
#include <dwmapi.h>
#include <wrl.h>
#include <string>
#include <vector>
#include "WebView2.h"

#pragma comment(lib, "Advapi32.lib")  // registry
#pragma comment(lib, "Ole32.lib")     // COM
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Dwmapi.lib")    // Mica / dark mode

// Some of these may be absent in older Windows SDK headers; define fallbacks so
// the shell still builds. The runtime call is gated separately at execution.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif

using namespace Microsoft::WRL;

static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static HANDLE g_server = nullptr;
static bool g_in_setup = false;
static char g_cockpit_url[PINBACK_URL_MAX] = {};

// Native menubar state.
static HMENU g_menubar = nullptr;
static HMENU g_viewMenu = nullptr;
static HMENU g_wsMenu = nullptr;
static std::vector<std::wstring> g_wsIds;   // menu index -> workspace id
static bool g_canGoBack = false;
static EventRegistrationToken g_msgToken = {};

// Theme override the user can pick from the View menu (Auto follows the OS).
enum ThemeMode { THEME_AUTO = 0, THEME_LIGHT = 1, THEME_DARK = 2 };
static ThemeMode g_theme = THEME_AUTO;

enum : UINT {
    ID_VIEW_BACK = 100,
    ID_VIEW_RELOAD,
    ID_VIEW_SERVER,
    ID_THEME_AUTO,
    ID_THEME_LIGHT,
    ID_THEME_DARK,
    ID_WS_BASE = 1000,   // workspace items: ID_WS_BASE + index
};

static bool HasUrlOverride(wchar_t* out, DWORD cap) {
    DWORD n = GetEnvironmentVariableW(L"PINBACK_URL", out, cap);
    return n > 0 && n < cap;
}

static std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

static void LoadCockpit(const wchar_t* url) {
    if (!g_webview || !url || !*url) return;
    g_in_setup = false;
    char narrow[PINBACK_URL_MAX];
    WideCharToMultiByte(CP_UTF8, 0, url, -1, narrow, sizeof narrow, nullptr, nullptr);
    strncpy_s(g_cockpit_url, narrow, _TRUNCATE);
    g_webview->Navigate(url);
}

static void LoadSetupPrefill(const char* prefill) {
    char setup_uri[PINBACK_URL_MAX + 16];
    if (pinback_setup_file_uri(setup_uri, sizeof setup_uri) != 0) return;
    g_in_setup = true;
    g_webview->Navigate(Utf8ToWide(setup_uri).c_str());
    if (prefill && *prefill) {
        std::wstring esc;
        for (const char* c = prefill; *c; ++c) {
            if (*c == '\\' || *c == '\'') esc += L'\\';
            esc += (wchar_t)(unsigned char)*c;
        }
        std::wstring js = L"document.getElementById('url').value='" + esc + L"';";
        g_webview->ExecuteScript(js.c_str(),
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
    }
}

static void StartServer();

static void BeginSession() {
    wchar_t envUrl[2048];
    if (HasUrlOverride(envUrl, ARRAYSIZE(envUrl))) {
        LoadCockpit(envUrl);
        return;
    }
    char url[PINBACK_URL_MAX];
    pinback_url_resolve(url, sizeof url, pinback_url_default());
    if (pinback_health_ok(url)) {
        LoadCockpit(Utf8ToWide(url).c_str());
        return;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    StartServer();
    pinback_url_resolve(url, sizeof url, pinback_url_default());
    if (pinback_health_ok(url)) {
        LoadCockpit(Utf8ToWide(url).c_str());
        return;
    }
    LoadSetupPrefill(url);
}

static std::wstring ServerPath() {
    if (const wchar_t* ovr = _wgetenv(L"PINBACK_SERVER_BIN"); ovr && *ovr)
        return ovr;
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (n > 0 && n < ARRAYSIZE(exe)) {
        std::wstring p(exe, n);
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            return p.substr(0, slash + 1) + L"pinback-server.exe";
    }
    return L"pinback-server.exe";
}

static void StartServer() {
    std::wstring exe = ServerPath();
    std::wstring cmd = L"\"" + exe + L"\" --bind 127.0.0.1:8088 --quiet";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring mutable_cmd = cmd;
    if (CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        g_server = pi.hProcess;
        CloseHandle(pi.hThread);
        char url[PINBACK_URL_MAX];
        pinback_url_resolve(url, sizeof url, pinback_url_default());
        for (int i = 0; i < 150 && !pinback_health_ok(url); i++) Sleep(200);
    }
}

static void StopServer() {
    if (g_server) {
        TerminateProcess(g_server, 0);
        CloseHandle(g_server);
        g_server = nullptr;
    }
}

// ----------------------------------------------------------------------------
// Theme: Mica backdrop + dark/light caption following the OS (or a manual View
// override), plus WebView2 PreferredColorScheme. Every DWM call is best-effort.
// ----------------------------------------------------------------------------
static bool SystemUsesDark() {
    HKEY h;
    DWORD v = 1, cb = sizeof(v), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &h) == ERROR_SUCCESS) {
        RegQueryValueExW(h, L"AppsUseLightTheme", nullptr, &type, (LPBYTE)&v, &cb);
        RegCloseKey(h);
    }
    return v == 0;   // AppsUseLightTheme: 0 = dark, 1 = light
}

static void ApplyTheme(HWND hWnd) {
    const bool dark = (g_theme == THEME_AUTO) ? SystemUsesDark()
                                              : (g_theme == THEME_DARK);

    BOOL useDark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));

    int backdrop = DWMSBT_MAINWINDOW;   // Mica behind the whole window
    DwmSetWindowAttribute(hWnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    if (g_webview) {
        ComPtr<ICoreWebView2_13> w13;
        if (SUCCEEDED(g_webview.As(&w13)) && w13) {
            ComPtr<ICoreWebView2Profile> prof;
            if (SUCCEEDED(w13->get_Profile(&prof)) && prof) {
                COREWEBVIEW2_PREFERRED_COLOR_SCHEME scheme =
                    (g_theme == THEME_AUTO) ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO
                  : (g_theme == THEME_DARK) ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK
                                            : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT;
                prof->put_PreferredColorScheme(scheme);
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Native menubar.
// ----------------------------------------------------------------------------
static void BuildMenuBar(HWND hWnd) {
    g_menubar = CreateMenu();

    g_wsMenu = CreatePopupMenu();   // populated live from the cockpit
    AppendMenuW(g_wsMenu, MF_GRAYED, 0, L"(loading…)");
    AppendMenuW(g_menubar, MF_POPUP, (UINT_PTR)g_wsMenu, L"&Workspaces");

    g_viewMenu = CreatePopupMenu();
    AppendMenuW(g_viewMenu, MF_STRING | MF_GRAYED, ID_VIEW_BACK, L"Previous workspace");
    AppendMenuW(g_viewMenu, MF_STRING, ID_VIEW_RELOAD, L"&Reload");
    AppendMenuW(g_viewMenu, MF_STRING, ID_VIEW_SERVER, L"Server &settings\u2026");
    AppendMenuW(g_viewMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_viewMenu, MF_STRING, ID_THEME_AUTO,  L"Theme: System");
    AppendMenuW(g_viewMenu, MF_STRING, ID_THEME_LIGHT, L"Theme: Light");
    AppendMenuW(g_viewMenu, MF_STRING, ID_THEME_DARK,  L"Theme: Dark");
    CheckMenuRadioItem(g_viewMenu, ID_THEME_AUTO, ID_THEME_DARK, ID_THEME_AUTO, MF_BYCOMMAND);
    AppendMenuW(g_menubar, MF_POPUP, (UINT_PTR)g_viewMenu, L"&View");

    SetMenu(hWnd, g_menubar);
}

// Minimal JSON string reader: s[i] must be at the opening quote; returns the
// unescaped value and leaves i just past the closing quote. Robust to the
// backslashes Windows paths produce ("\\") and to \uXXXX.
static std::wstring JsonStr(const std::wstring& s, size_t& i) {
    std::wstring out;
    if (i >= s.size() || s[i] != L'"') return out;
    ++i;
    while (i < s.size() && s[i] != L'"') {
        wchar_t c = s[i++];
        if (c == L'\\' && i < s.size()) {
            wchar_t e = s[i++];
            switch (e) {
                case L'n': out += L'\n'; break;
                case L't': out += L'\t'; break;
                case L'r': out += L'\r'; break;
                case L'b': out += L'\b'; break;
                case L'f': out += L'\f'; break;
                case L'u': {
                    wchar_t code = 0;
                    for (int k = 0; k < 4 && i < s.size(); ++k) {
                        wchar_t hgt = s[i++]; code <<= 4;
                        if (hgt >= L'0' && hgt <= L'9') code |= (hgt - L'0');
                        else if (hgt >= L'a' && hgt <= L'f') code |= (10 + hgt - L'a');
                        else if (hgt >= L'A' && hgt <= L'F') code |= (10 + hgt - L'A');
                    }
                    out += code; break;
                }
                default: out += e; break;   // \" \\ \/ and any other
            }
        } else {
            out += c;
        }
    }
    if (i < s.size() && s[i] == L'"') ++i;
    return out;
}

static void SkipWs(const std::wstring& s, size_t& i) {
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\n' ||
                            s[i] == L'\r' || s[i] == L':' || s[i] == L',')) ++i;
}

struct WsEntry { std::wstring id, label; };

// Parse the {type:'workspaces', activeId, canGoBack, workspaces:[{id,label,path}]}
// frame the page posts. Key lookups are token searches (our keys are distinctive);
// each workspace object holds only string values, so no nesting handling needed.
static void ParseWorkspaceMessage(const std::wstring& s,
                                  std::vector<WsEntry>& out,
                                  std::wstring& activeId, bool& canGoBack) {
    out.clear(); activeId.clear(); canGoBack = false;

    if (size_t p = s.find(L"\"activeId\""); p != std::wstring::npos) {
        p += 10; SkipWs(s, p);
        if (p < s.size() && s[p] == L'"') activeId = JsonStr(s, p);
    }
    if (size_t p = s.find(L"\"canGoBack\""); p != std::wstring::npos) {
        p += 11; SkipWs(s, p);
        canGoBack = (s.compare(p, 4, L"true") == 0);
    }
    size_t p = s.find(L"\"workspaces\"");
    if (p == std::wstring::npos) return;
    p += 12; SkipWs(s, p);
    if (p >= s.size() || s[p] != L'[') return;
    ++p;
    while (p < s.size()) {
        SkipWs(s, p);
        if (p < s.size() && s[p] == L']') break;
        if (p < s.size() && s[p] == L'{') {
            ++p;
            WsEntry w;
            while (p < s.size() && s[p] != L'}') {
                SkipWs(s, p);
                if (p < s.size() && s[p] == L'"') {
                    std::wstring key = JsonStr(s, p);
                    SkipWs(s, p);
                    std::wstring val = (p < s.size() && s[p] == L'"') ? JsonStr(s, p) : L"";
                    if (key == L"id") w.id = val;
                    else if (key == L"label") w.label = val;
                } else if (p < s.size() && s[p] != L'}') {
                    ++p;
                }
            }
            if (p < s.size() && s[p] == L'}') ++p;
            if (!w.id.empty()) out.push_back(w);
        } else {
            ++p;   // unexpected token; skip
        }
    }
}

static void OnSetupMessage(const std::wstring& json) {
    std::wstring action, url;
    if (size_t p = json.find(L"\"action\""); p != std::wstring::npos) {
        p += 8; SkipWs(json, p);
        if (p < json.size() && json[p] == L'"') action = JsonStr(json, p);
    }
    if (size_t p = json.find(L"\"url\""); p != std::wstring::npos) {
        p += 5; SkipWs(json, p);
        if (p < json.size() && json[p] == L'"') url = JsonStr(json, p);
    }
    if (url.empty()) return;
    char narrow[PINBACK_URL_MAX];
    WideCharToMultiByte(CP_UTF8, 0, url.c_str(), -1, narrow, sizeof narrow, nullptr, nullptr);
    if (action == L"test") {
        if (pinback_health_ok(narrow))
            g_webview->ExecuteScript(
                L"document.getElementById('status').textContent='Server is reachable.';"
                L"document.getElementById('status').className='ok';", nullptr);
        else
            g_webview->ExecuteScript(
                L"document.getElementById('status').textContent='Cannot reach server at that URL.';"
                L"document.getElementById('status').className='err';", nullptr);
        return;
    }
    if (action == L"connect") {
        pinback_url_save(narrow);
        if (pinback_health_ok(narrow))
            LoadCockpit(url.c_str());
        else
            g_webview->ExecuteScript(
                L"document.getElementById('status').textContent='Saved, but server still unreachable.';"
                L"document.getElementById('status').className='err';", nullptr);
    }
}

static void OnWorkspacesMessage(HWND hWnd, const std::wstring& json) {
    std::vector<WsEntry> list;
    std::wstring activeId;
    ParseWorkspaceMessage(json, list, activeId, g_canGoBack);

    // Rebuild the Workspaces popup from scratch.
    HMENU fresh = CreatePopupMenu();
    g_wsIds.clear();
    if (list.empty()) {
        AppendMenuW(fresh, MF_GRAYED, 0, L"(no workspaces)");
    } else {
        int activeIdx = -1;
        for (size_t k = 0; k < list.size(); ++k) {
            const std::wstring text = list[k].label.empty() ? list[k].id : list[k].label;
            AppendMenuW(fresh, MF_STRING, ID_WS_BASE + (UINT)k, text.c_str());
            if (list[k].id == activeId) activeIdx = (int)k;
            g_wsIds.push_back(list[k].id);
        }
        if (activeIdx >= 0)
            CheckMenuRadioItem(fresh, 0, (UINT)list.size() - 1, (UINT)activeIdx, MF_BYPOSITION);
    }
    ModifyMenuW(g_menubar, 0, MF_BYPOSITION | MF_POPUP, (UINT_PTR)fresh, L"&Workspaces");
    if (g_wsMenu) DestroyMenu(g_wsMenu);
    g_wsMenu = fresh;

    EnableMenuItem(g_viewMenu, ID_VIEW_BACK,
                   MF_BYCOMMAND | (g_canGoBack ? MF_ENABLED : MF_GRAYED));
    DrawMenuBar(hWnd);
}

static std::wstring JsEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'\'') { o += L'\\'; o += c; }
        else if (c == L'\n') o += L"\\n";
        else if (c == L'\r') {}
        else o += c;
    }
    return o;
}

static void RunPinback(const std::wstring& call) {
    if (!g_webview) return;
    const std::wstring js = L"window.pinback&&window.pinback." + call;
    g_webview->ExecuteScript(js.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
}

using CreateWebViewEnvironmentWithOptionsInternal_t = HRESULT(STDMETHODCALLTYPE *)(
    bool, int, PCWSTR, IUnknown *,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

static const wchar_t kClientStateKey[] =
    L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

static bool RuntimeInstallRoot(wchar_t* out, DWORD cch) {
    static const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    for (HKEY root : roots) {
        HKEY h;
        if (RegOpenKeyExW(root, kClientStateKey, 0, KEY_READ | KEY_WOW64_32KEY, &h) == ERROR_SUCCESS) {
            DWORD cb = cch * sizeof(wchar_t), type = 0;
            LONG r = RegQueryValueExW(h, L"EBWebView", nullptr, &type, (LPBYTE)out, &cb);
            RegCloseKey(h);
            if (r == ERROR_SUCCESS && type == REG_SZ && out[0]) return true;
        }
    }
    return false;
}

static const wchar_t* UserDataDir() {
    static wchar_t d[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", d, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) lstrcpyW(d, L".");
    lstrcatW(d, L"\\PinbackShell");
    CreateDirectoryW(d, nullptr);
    return d;
}

static HRESULT CreateEnvironmentBuiltin(
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler) {
    wchar_t root[1024];
    if (!RuntimeInstallRoot(root, ARRAYSIZE(root))) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
#if defined(_M_ARM64)
    const wchar_t* arch = L"arm64";
#elif defined(_M_IX86)
    const wchar_t* arch = L"x86";
#else
    const wchar_t* arch = L"x64";
#endif
    wchar_t dll[1200];
    lstrcpyW(dll, root);
    lstrcatW(dll, L"\\EBWebView\\");
    lstrcatW(dll, arch);
    lstrcatW(dll, L"\\EmbeddedBrowserWebView.dll");
    HMODULE mod = LoadLibraryW(dll);
    if (!mod) return HRESULT_FROM_WIN32(GetLastError());
    auto fn = reinterpret_cast<CreateWebViewEnvironmentWithOptionsInternal_t>(
        GetProcAddress(mod, "CreateWebViewEnvironmentWithOptionsInternal"));
    if (!fn) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return fn(true, 0, UserDataDir(), nullptr, handler);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (g_controller) {
            RECT bounds;
            GetClientRect(hWnd, &bounds);
            g_controller->put_Bounds(bounds);
        }
        return 0;
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        if (id >= ID_WS_BASE && id < ID_WS_BASE + (UINT)g_wsIds.size()) {
            RunPinback(L"selectWorkspace('" + JsEscape(g_wsIds[id - ID_WS_BASE]) + L"')");
            return 0;
        }
        switch (id) {
        case ID_VIEW_BACK:   RunPinback(L"back()"); return 0;
        case ID_VIEW_RELOAD: if (g_webview) g_webview->Reload(); return 0;
        case ID_VIEW_SERVER:
            LoadSetupPrefill(g_cockpit_url[0] ? g_cockpit_url : pinback_url_default());
            return 0;
        case ID_THEME_AUTO:  case ID_THEME_LIGHT: case ID_THEME_DARK:
            g_theme = (id == ID_THEME_AUTO) ? THEME_AUTO
                    : (id == ID_THEME_LIGHT) ? THEME_LIGHT : THEME_DARK;
            CheckMenuRadioItem(g_viewMenu, ID_THEME_AUTO, ID_THEME_DARK, id, MF_BYCOMMAND);
            ApplyTheme(hWnd);
            return 0;
        }
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (g_theme == THEME_AUTO && lParam &&
            CompareStringOrdinal(reinterpret_cast<LPCWSTR>(lParam), -1,
                                 L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL) {
            ApplyTheme(hWnd);
        }
        return 0;
    case WM_DESTROY:
        StopServer();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const wchar_t kClass[] = L"PinbackShell";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);

    HWND hWnd = CreateWindowExW(
        0, kClass, L"Pinback", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        nullptr, nullptr, hInstance, nullptr);

    BuildMenuBar(hWnd);    // before ShowWindow so the client rect excludes the bar
    ApplyTheme(hWnd);      // Mica + dark caption up front (best-effort on Win10)
    ShowWindow(hWnd, nCmdShow);

    HRESULT hr = CreateEnvironmentBuiltin(
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hWnd](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
                if (!env) return S_OK;
                env->CreateCoreWebView2Controller(
                    hWnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hWnd](HRESULT, ICoreWebView2Controller* controller) -> HRESULT {
                            if (!controller) return S_OK;
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);

                            // Transparent backdrop so Mica can show through any
                            // transparent page regions; pre-Controller2 runtimes
                            // simply skip this.
                            ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(g_controller.As(&c2)) && c2) {
                                COREWEBVIEW2_COLOR clear = { 0, 0, 0, 0 };  // A,R,G,B
                                c2->put_DefaultBackgroundColor(clear);
                            }

                            // web -> native: workspace list / active / canGoBack.
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [hWnd](ICoreWebView2*,
                                           ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR json = nullptr;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                                            std::wstring msg(json);
                                            if (msg.find(L"\"type\":\"pinback-host\"") != std::wstring::npos &&
                                                msg.find(L"openSetup") != std::wstring::npos)
                                                LoadSetupPrefill(g_cockpit_url[0] ? g_cockpit_url : pinback_url_default());
                                            else if (msg.find(L"pinback-setup") != std::wstring::npos)
                                                OnSetupMessage(msg);
                                            else if (!g_in_setup)
                                                OnWorkspacesMessage(hWnd, msg);
                                            CoTaskMemFree(json);
                                        }
                                        return S_OK;
                                    }).Get(), &g_msgToken);

                            ApplyTheme(hWnd);   // now that g_webview exists, set color scheme too

                            RECT bounds;
                            GetClientRect(hWnd, &bounds);
                            g_controller->put_Bounds(bounds);
                            BeginSession();
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        MessageBoxW(hWnd,
            L"Failed to create the WebView2 environment.\n"
            L"Install the Microsoft Edge WebView2 Runtime and retry.",
            L"Pinback", MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
