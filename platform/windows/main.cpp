// Pinback Windows shell: a Win32 window hosting a WebView2 control. WebView2 is
// powered by the system-installed Microsoft Edge (Chromium) "Evergreen" runtime,
// so no browser engine is bundled. This build also ships NO WebView2Loader.dll:
// it locates the runtime via the registry and calls its internal environment
// entry point directly (the same technique the webview/webview library uses), so
// the deliverable is a single self-contained ~tens-of-KB exe. WRL (Windows SDK,
// header-only) is kept for the async COM callbacks.
#include <windows.h>
#include <wrl.h>
#include "WebView2.h"

#pragma comment(lib, "Advapi32.lib")  // registry
#pragma comment(lib, "Ole32.lib")     // COM

using namespace Microsoft::WRL;

static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;

static LPCWSTR StartUrl() {
    static wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(L"PINBACK_URL", buf, ARRAYSIZE(buf));
    return (n > 0 && n < ARRAYSIZE(buf)) ? buf : L"http://127.0.0.1:18192";
}

// ---- Built-in WebView2 loader (no WebView2Loader.dll) ----------------------
// The Evergreen runtime exports this internal entry point; the public
// CreateCoreWebView2EnvironmentWithOptions in WebView2Loader.dll forwards to it.
using CreateWebViewEnvironmentWithOptionsInternal_t = HRESULT(STDMETHODCALLTYPE *)(
    bool, int /*runtime kind: 0 = installed Evergreen*/, PCWSTR /*user data dir*/,
    IUnknown * /*env options*/,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

// EdgeUpdate stores the runtime install root under the stable-channel client key.
static const wchar_t kClientStateKey[] =
    L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

static bool RuntimeInstallRoot(wchar_t* out, DWORD cch) {
    // EdgeUpdate registers under the 32-bit registry view (KEY_WOW64_32KEY).
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
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1)); // app.rc icon
    RegisterClassW(&wc);

    HWND hWnd = CreateWindowExW(
        0, kClass, L"Pinback", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hWnd, nCmdShow);

    // Create the WebView2 environment (via the built-in loader), then a controller
    // parented to our HWND, then navigate. Each step is an async WRL COM callback.
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
