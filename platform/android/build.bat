@echo off
REM Build the Android shell APK. Requires JDK 17+ and ANDROID_HOME.
REM Usage:  build.bat [debug|release|both]   (default debug)
setlocal
set VARIANT=%1
if "%VARIANT%"=="" set VARIANT=debug

cd /d "%~dp0"

if /I "%VARIANT%"=="debug" (
    call gradlew.bat assembleDebug --no-daemon || exit /b 1
    echo.
    echo Built: app\build\outputs\apk\debug\app-debug.apk
    goto :done
)
if /I "%VARIANT%"=="release" (
    call gradlew.bat assembleRelease --no-daemon || exit /b 1
    echo.
    echo Built: app\build\outputs\apk\release\app-release-unsigned.apk
    goto :done
)
if /I "%VARIANT%"=="both" (
    call gradlew.bat assembleDebug assembleRelease --no-daemon || exit /b 1
    echo.
    echo Built debug + release APKs under app\build\outputs\apk\
    goto :done
)

echo usage: build.bat [debug^|release^|both]
exit /b 2

:done
endlocal
