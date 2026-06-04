# Remote smoke test for pinback-shell.exe against a Mac-hosted pinback-server.
param(
    [Parameter(Mandatory = $true)]
    [string]$MacHost,
    [int]$Port = 8088,
    [string]$Exe = "C:\Users\mac\pinback\platform\windows\build\Release\pinback-shell.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) {
    Write-Host "EXE_MISSING $Exe"
    exit 1
}
Write-Host "EXE_OK"

$url = "http://${MacHost}:$Port/healthz"
$h = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 8 $url).Content.Trim()
Write-Host "REMOTE_HEALTHZ $h"

$env:PINBACK_URL = "http://${MacHost}:$Port"
$p = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 6
if (-not $p.HasExited) {
    Write-Host "SHELL_OK pid=$($p.Id)"
    Stop-Process -Id $p.Id -Force
    Write-Host "SHELL_CLEANUP OK"
} else {
    Write-Host "SHELL_FAIL exit=$($p.ExitCode)"
    exit 1
}
