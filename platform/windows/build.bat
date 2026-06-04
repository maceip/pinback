@echo off
REM Convenience build for the Windows shell. Requires CMake + MSVC (VS 2022).
REM Usage:  build.bat [Debug|Release]   (default Release)
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

cmake -S "%~dp0" -B "%~dp0build" -A x64 || exit /b 1
cmake --build "%~dp0build" --config %CONFIG% || exit /b 1

echo.
echo Built: %~dp0build\%CONFIG%\pinback-shell.exe
echo Run:   set PINBACK_URL=http://127.0.0.1:18192 ^&^& %~dp0build\%CONFIG%\pinback-shell.exe
endlocal
