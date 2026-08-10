# Minima per-user uninstall. Removes the app, shortcut, browser registration, and
# uninstall entry. Browsing data in %LOCALAPPDATA%\Minima is kept unless -RemoveData.
# Usage:  powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-RemoveData]
param([switch]$RemoveData)
$ErrorActionPreference = 'SilentlyContinue'

Get-Process minima | Stop-Process -Force
Start-Sleep -Milliseconds 500

$dest = "$env:LOCALAPPDATA\Programs\Minima"
$exe = Join-Path $dest 'minima.exe'
if (Test-Path $exe) { & $exe --unregister }

Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'Minima.lnk') -Force
Remove-Item 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Minima' -Recurse -Force
if ($RemoveData) { Remove-Item "$env:LOCALAPPDATA\Minima" -Recurse -Force }

# Delete the install dir last (this script lives inside it).
Start-Process powershell -ArgumentList "-NoProfile -Command Start-Sleep 1; Remove-Item -Recurse -Force `"$dest`"" -WindowStyle Hidden
Write-Host 'Minima uninstalled.'
if (-not $RemoveData) { Write-Host "Browsing data kept at $env:LOCALAPPDATA\Minima (rerun with -RemoveData to delete)." }
