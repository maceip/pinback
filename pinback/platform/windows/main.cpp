// Pinback Windows shell: a Win32 window hosting a WebView2 control. WebView2 is
// powered by the system-installed Microsoft Edge (Chromium) "Evergreen" runtime,
// so no browser engine is bundled in the binary. Uses only WRL (ships with the
// Windows SDK) for the async COM callbacks — no extra glue libraries.
//
// Embedding model (how the shell "loads pinback"):
//   - If PINBACK_URL is set, load it verbatim and spawn nothing (dev/remote).
//   - Otherwise the shell IS the launcher: it spawns the bundled
//     `pinback-server.exe` child on 127.0.0.1:8088, waits for /healthz, then
//     loads it, and terminates the child on window close. A novice runs one
//     exe, no console — the cockpit backend + UI come up underneath it.
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wrl.h>
#include <string>
#include "WebView2.h"

#pragma comment(lib, "ws2_32.lib")

using namespace Microsoft::WRL;

static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static HANDLE g_server = nullptr;     // child pinback-server.exe, if we spawned it

static constexpr int kPort = 8088;

// True iff the user set PINBACK_URL (we then load it and never spawn).
static bool HasUrlOverride(wchar_t* out, DWORD cap) {
    DWORD n = GetEnvironmentVariableW(L"PINBACK_URL", out, cap);
    return n > 0 && n < cap;
}

static LPCWSTR StartUrl() {
    static wchar_t buf[2048];
    if (HasUrlOverride(buf, ARRAYSIZE(buf))) return buf;
    return L"http://127.0.0.1:8088";
}

// One HTTP GET /healthz over winsock; true iff the reply starts "HTTP/.. 200".
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

// Locate pinback-server.exe next to this exe; fall back to PATH (bare name).
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

// Spawn the loopback server and block (pumping nothing) until /healthz answers.
static void StartServer() {
    std::wstring exe = ServerPath();
    std::wstring cmd = L"\"" + exe + L"\" --bind 127.0.0.1:8088 --quiet";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring mutable_cmd = cmd;   // CreateProcessW may modify the buffer
    if (CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        g_server = pi.hProcess;
        CloseHandle(pi.hThread);
        for (int i = 0; i < 150 && !HealthOk(); i++) Sleep(200);  // up to ~30s
    }
}

static void StopServer() {
    if (g_server) {
        TerminateProcess(g_server, 0);
        CloseHandle(g_server);
        g_server = nullptr;
    }
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
    const wchar_t kClass[] = L"PinbackShell";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1)); // app.rc icon
    RegisterClassW(&wc);

    HWND hWnd = CreateWindowExW(
        0, kClass, L"Pinback", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hWnd, nCmdShow);

    // Self-host the cockpit on loopback unless the user pointed us elsewhere.
    wchar_t urlBuf[2048];
    if (!HasUrlOverride(urlBuf, ARRAYSIZE(urlBuf))) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        StartServer();
    }

    // Create the WebView2 environment, then a controller parented to our HWND,
    // then navigate. Each step is an async COM callback delivered via WRL.
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hWnd](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
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
