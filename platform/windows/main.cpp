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
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wrl.h>
#include <string>
#include "WebView2.h"

#pragma comment(lib, "Advapi32.lib")  // registry
#pragma comment(lib, "Ole32.lib")     // COM
#pragma comment(lib, "ws2_32.lib")

using namespace Microsoft::WRL;

static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static HANDLE g_server = nullptr;

static constexpr int kPort = 8088;

static bool HasUrlOverride(wchar_t* out, DWORD cap) {
    DWORD n = GetEnvironmentVariableW(L"PINBACK_URL", out, cap);
    return n > 0 && n < cap;
}

static LPCWSTR StartUrl() {
    static wchar_t buf[2048];
    if (HasUrlOverride(buf, ARRAYSIZE(buf))) return buf;
    return L"http://127.0.0.1:8088";
}

static bool HealthOk() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(kPort);
    InetPtonW(AF_INET, L"127.0.0.1", &sa.sin_addr);
    bool ok = false;
    if (connect(s, (sockaddr*)&sa, sizeof(sa)) == 0) {
        const char* req = "GET /healthz HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
        send(s, req, (int)strlen(req), 0);
        char buf[64] = {};
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        ok = n > 12 && memcmp(buf, "HTTP/", 5) == 0 && strstr(buf, " 200") != nullptr;
    }
    closesocket(s);
    return ok;
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
        for (int i = 0; i < 150 && !HealthOk(); i++) Sleep(200);
    }
}

static void StopServer() {
    if (g_server) {
        TerminateProcess(g_server, 0);
        CloseHandle(g_server);
        g_server = nullptr;
    }
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
    ShowWindow(hWnd, nCmdShow);

    wchar_t urlBuf[2048];
    if (!HasUrlOverride(urlBuf, ARRAYSIZE(urlBuf))) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        StartServer();
    }

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
                            RECT bounds;
                            GetClientRect(hWnd, &bounds);
                            g_controller->put_Bounds(bounds);
                            g_webview->Navigate(StartUrl());
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
