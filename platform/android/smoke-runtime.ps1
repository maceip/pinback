# Optional runtime smoke: WSL pinback-server + adb install/launch.
# Requires a running emulator/device (adb devices). Emulator reaches the host
# server via http://10.0.2.2:8088 (BuildConfig default).
param(
    [string]$Root = "C:\Users\mac\pinback",
    [int]$Port = 8088
)

$ErrorActionPreference = "Stop"

$adb = Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools\adb.exe"
if (-not (Test-Path $adb)) {
    Write-Host "ADB_MISSING"
    exit 1
}

$devices = & $adb devices | Select-String "device$"
if (-not $devices) {
    Write-Host "ADB_NO_DEVICE"
    exit 0
}
Write-Host "ADB_DEVICE OK"

$wslScript = @"
set -euo pipefail
cd /mnt/c/Users/mac/pinback
if [ ! -x ./pinback-server ]; then make pinback-server CFLAGS='-O2 -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Isrc' >/dev/null; fi
if ! curl -sf http://127.0.0.1:$Port/healthz >/dev/null 2>&1; then
  ./pinback-server --bind 0.0.0.0:$Port --quiet &
  sleep 2
fi
curl -sf http://127.0.0.1:$Port/healthz
"@

wsl -d Ubuntu-24.04 -- bash -lc $wslScript
if ($LASTEXITCODE -ne 0) {
    Write-Host "WSL_SERVER_FAIL"
    exit 1
}
Write-Host "WSL_SERVER_OK"

$apk = Join-Path $Root "platform\android\app\build\outputs\apk\debug\app-debug.apk"
& $adb install -r $apk | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "ADB_INSTALL_FAIL"
    exit 1
}
Write-Host "ADB_INSTALL_OK"

& $adb shell am start -n "com.pinback.shell/.MainActivity" | Out-Null
Start-Sleep -Seconds 5

$top = & $adb shell dumpsys activity activities | Select-String -Pattern "com.pinback.shell" | Select-Object -First 1
if ($top) {
    Write-Host "ADB_ACTIVITY_OK"
} else {
    Write-Host "ADB_ACTIVITY_FAIL"
    exit 1
}
