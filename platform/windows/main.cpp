// Pinback Windows shell: a Win32 window hosting a WebView2 control. WebView2 is
// powered by the system-installed Microsoft Edge (Chromium) "Evergreen" runtime,
// so no browser engine is bundled in the binary. Uses only WRL (ships with the
// Windows SDK) for the async COM callbacks — no extra glue libraries.
#include <windows.h>
#include <wrl.h>
#include "WebView2.h"

using namespace Microsoft::WRL;

static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;

static LPCWSTR StartUrl() {
    static wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(L"PINBACK_URL", buf, ARRAYSIZE(buf));
    return (n > 0 && n < ARRAYSIZE(buf)) ? buf : L"http://127.0.0.1:18192";
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
