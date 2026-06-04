cmake_minimum_required(VERSION 3.24)
project(pinback_shell CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch the WebView2 SDK from NuGet for its HEADERS only (the COM interface
# declarations). We do NOT link WebView2Loader — the shell locates the system
# runtime itself (see main.cpp), so the build produces a single self-contained
# exe with no DLL to ship. Bump WEBVIEW2_VERSION to track the newest SDK headers.
include(FetchContent)
set(WEBVIEW2_VERSION "1.0.3967.48" CACHE STRING "Microsoft.Web.WebView2 NuGet version")
FetchContent_Declare(webview2
    URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"
    DOWNLOAD_NAME "webview2-${WEBVIEW2_VERSION}.zip")
FetchContent_MakeAvailable(webview2)

add_executable(pinback-shell WIN32 main.cpp app.manifest app.rc)
target_include_directories(pinback-shell PRIVATE "${webview2_SOURCE_DIR}/build/native/include")
# This is a Unicode app — all Win32 calls are the ...W variants.
target_compile_definitions(pinback-shell PRIVATE UNICODE _UNICODE)
# Only system libs (Advapi32/Ole32 are pulled in via #pragma comment in main.cpp).
target_link_libraries(pinback-shell PRIVATE user32)

# Size-oriented Release flags: optimize for size, function-level linking, then
# fold/drop unreferenced code. Dynamic CRT is kept on purpose (/MT is ~10x bigger).
if(MSVC)
    target_compile_options(pinback-shell PRIVATE $<$<CONFIG:Release>:/O1 /Os /Gy>)
    target_link_options(pinback-shell PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
endif()
