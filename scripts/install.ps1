# Minima per-user install (no admin needed).
# Copies the built exe to %LOCALAPPDATA%\Programs\Minima, creates a Start-menu shortcut,
# registers Minima as a browser candidate (Default apps), and adds an uninstall entry.
# Usage:  powershell -ExecutionPolicy Bypass -File scripts\install.ps1
$ErrorActionPreference = 'Stop'

$src = Join-Path $PSScriptRoot '..\build\minima.exe'
if (-not (Test-Path $src)) { Write-Error 'build\minima.exe not found - run build.bat first.' }

$dest = "$env:LOCALAPPDATA\Programs\Minima"
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item $src (Join-Path $dest 'minima.exe') -Force
Copy-Item (Join-Path $PSScriptRoot 'uninstall.ps1') (Join-Path $dest 'uninstall.ps1') -Force
$exe = Join-Path $dest 'minima.exe'

# Start-menu shortcut.
$startMenu = [Environment]::GetFolderPath('Programs')
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut((Join-Path $startMenu 'Minima.lnk'))
$lnk.TargetPath = $exe
$lnk.WorkingDirectory = $dest
$lnk.Description = 'Minima - an ultra-fast, minimal browser with on-device AI'
$lnk.Save()

# Register as a browser candidate (per-user; shows up in Windows' Default apps).
& $exe --register

# Uninstall entry (Settings > Apps).
$un = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Minima'
New-Item -Path $un -Force | Out-Null
Set-ItemProperty $un DisplayName 'Minima'
Set-ItemProperty $un DisplayIcon "$exe,0"
Set-ItemProperty $un Publisher 'Minima'
Set-ItemProperty $un InstallLocation $dest
Set-ItemProperty $un UninstallString "powershell -ExecutionPolicy Bypass -File `"$dest\uninstall.ps1`""
Set-ItemProperty $un NoModify 1 -Type DWord
Set-ItemProperty $un NoRepair 1 -Type DWord

Write-Host "Installed to $dest"
Write-Host 'Start-menu shortcut created; Minima is registered in Windows Default apps.'
Write-Host 'To make it your default browser: Settings > Apps > Default apps > Minima.'
