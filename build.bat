@echo off
setlocal
rem Build Minima (C++ / WebView2). Requires VS 2022 Build Tools.

if defined VCToolsInstallDir goto :build
set "VSPATH=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath 2^>nul`) do set "VSPATH=%%i"
)
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
  echo Visual Studio Build Tools not found.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul

:build

if not exist build mkdir build

rc /nologo /fo build\app.res src\app.rc
if %errorlevel% neq 0 (
  echo RC FAILED
  exit /b 1
)

cl /nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE ^
  /I vendor\webview2-pkg\build\native\include ^
  src\main.cpp build\app.res ^
  /Fo:build\ /Fe:build\minima.exe ^
  /link /SUBSYSTEM:WINDOWS ^
  vendor\webview2-pkg\build\native\x64\WebView2LoaderStatic.lib

if %errorlevel% neq 0 (
  echo BUILD FAILED
  exit /b 1
)
echo BUILD OK: build\minima.exe
