cmake_minimum_required(VERSION 3.24)
project(pinback_shell CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pull the WebView2 SDK straight from NuGet (a .nupkg is a zip). Nothing about
# the engine is vendored in this repo; only the small loader/headers are fetched
# at configure time. Bump WEBVIEW2_VERSION to track the newest SDK.
include(FetchContent)
set(WEBVIEW2_VERSION "1.0.3967.48" CACHE STRING "Microsoft.Web.WebView2 NuGet version")
FetchContent_Declare(webview2
    URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"
    DOWNLOAD_NAME "webview2-${WEBVIEW2_VERSION}.zip")
FetchContent_MakeAvailable(webview2)

set(WV2_NATIVE "${webview2_SOURCE_DIR}/build/native")

add_executable(pinback-shell WIN32 main.cpp)
target_include_directories(pinback-shell PRIVATE "${WV2_NATIVE}/include")

# Static loader => no WebView2Loader.dll to ship next to the exe.
target_link_libraries(pinback-shell PRIVATE
    "${WV2_NATIVE}/x64/WebView2LoaderStatic.lib"
    ole32 oleaut32 user32 shlwapi version)
