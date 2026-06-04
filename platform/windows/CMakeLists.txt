cmake_minimum_required(VERSION 3.24)
project(pinback_shell CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pull the WebView2 SDK straight from NuGet (a .nupkg is a zip). Nothing about
# the engine is vendored in this repo; only the small loader/headers are fetched
# at configure time. Bump WEBVIEW2_VERSION to track the newest SDK.
include(FetchContent)
set(WEBVIEW2_VERSION "1.0.3967.48" CACHE STRING "Microsoft.Web.WebView2 NuGet version")
option(WEBVIEW2_STATIC_LOADER
    "Link WebView2LoaderStatic.lib (no DLL to ship) instead of the loader DLL" OFF)

FetchContent_Declare(webview2
    URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"
    DOWNLOAD_NAME "webview2-${WEBVIEW2_VERSION}.zip")
FetchContent_MakeAvailable(webview2)

set(WV2_NATIVE "${webview2_SOURCE_DIR}/build/native")

# Map the build architecture to the SDK's lib subfolder (x64 / x86 / arm64).
if(CMAKE_GENERATOR_PLATFORM)
    string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _arch)
else()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _arch)
endif()
if(_arch STREQUAL "win32" OR _arch STREQUAL "x86")
    set(WV2_ARCH "x86")
elseif(_arch STREQUAL "arm64" OR _arch STREQUAL "aarch64")
    set(WV2_ARCH "arm64")
else()
    set(WV2_ARCH "x64")
endif()

add_executable(pinback-shell WIN32 main.cpp app.manifest)
target_include_directories(pinback-shell PRIVATE "${WV2_NATIVE}/include")

if(WEBVIEW2_STATIC_LOADER)
    # Smaller deployment (no DLL), but pulls in extra system libs.
    target_link_libraries(pinback-shell PRIVATE
        "${WV2_NATIVE}/${WV2_ARCH}/WebView2LoaderStatic.lib"
        ole32 oleaut32 user32 shlwapi version)
else()
    # Default: link the import lib and drop WebView2Loader.dll next to the exe.
    target_link_libraries(pinback-shell PRIVATE
        "${WV2_NATIVE}/${WV2_ARCH}/WebView2Loader.dll.lib"
        ole32 oleaut32 user32 shlwapi version)
    add_custom_command(TARGET pinback-shell POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WV2_NATIVE}/${WV2_ARCH}/WebView2Loader.dll"
            "$<TARGET_FILE_DIR:pinback-shell>"
        COMMENT "Copying WebView2Loader.dll next to pinback-shell.exe")
endif()
