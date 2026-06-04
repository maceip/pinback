# Verify debug/release APK artifacts after assembleDebug/assembleRelease.
# Called by platform/smoke-test.sh over SSH on the Windows build host.
param(
    [string]$Root = "C:\Users\mac\pinback",
    [ValidateSet("debug", "release")]
    [string]$Variant = "debug"
)

$ErrorActionPreference = "Stop"

$apk = switch ($Variant) {
    "debug"   { Join-Path $Root "platform\android\app\build\outputs\apk\debug\app-debug.apk" }
    "release" { Join-Path $Root "platform\android\app\build\outputs\apk\release\app-release-unsigned.apk" }
}

if (-not (Test-Path $apk)) {
    Write-Host "APK_MISSING $apk"
    exit 1
}

$len = (Get-Item $apk).Length
Write-Host "APK_SIZE $len"

$aapt = Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA "Android\Sdk\build-tools") `
    -Recurse -Filter "aapt.exe" -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending |
    Select-Object -First 1

if (-not $aapt) {
    Write-Host "AAPT_MISSING"
    exit 1
}

$badging = & $aapt.FullName dump badging $apk 2>&1 | Select-String "package: name="
Write-Host "APK_BADGING $badging"

if ("$badging" -notmatch "com\.pinback\.shell") {
    Write-Host "APK_PACKAGE_FAIL"
    exit 1
}

Write-Host "APK_VERIFY_OK"
