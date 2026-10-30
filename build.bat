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

rem SQLite (vendored amalgamation, public domain) backs Memory search and browser import.
rem It never changes between builds, so compile it once and reuse the object. Built for
rem size, with features Minima never uses left out - about 480 KB smaller; its queries are tiny.
rem (Delete build\sqlite3.obj after changing these flags.)
if not exist build\sqlite3.obj (
  cl /nologo /c /TC /O1 /Gy /MT /W0 /DSQLITE_ENABLE_FTS5 /DSQLITE_OMIT_LOAD_EXTENSION /DSQLITE_DQS=0 ^
    /DSQLITE_DEFAULT_MEMSTATUS=0 /DSQLITE_OMIT_DEPRECATED /DSQLITE_OMIT_JSON /DSQLITE_OMIT_SHARED_CACHE ^
    /DSQLITE_OMIT_EXPLAIN /DSQLITE_OMIT_AUTHORIZATION /DSQLITE_OMIT_PROGRESS_CALLBACK /DSQLITE_OMIT_TCL_VARIABLE ^
    /DSQLITE_OMIT_COMPLETE /DSQLITE_OMIT_GET_TABLE /DSQLITE_OMIT_UTF16 /DSQLITE_OMIT_TRACE ^
    /DSQLITE_OMIT_INTROSPECTION_PRAGMAS /DSQLITE_OMIT_DECLTYPE /DSQLITE_UNTESTABLE ^
    /DSQLITE_OMIT_COMPILEOPTION_DIAGS vendor\sqlite\sqlite3.c /Fo:build\sqlite3.obj
  if errorlevel 1 (
    echo SQLITE BUILD FAILED
    exit /b 1
  )
)

cl /nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE ^
  /I vendor\webview2-pkg\build\native\include /I vendor\sqlite ^
  src\main.cpp build\app.res build\sqlite3.obj ^
  /Fo:build\ /Fe:build\minima.exe ^
  /link /SUBSYSTEM:WINDOWS ^
  vendor\webview2-pkg\build\native\x64\WebView2LoaderStatic.lib

if %errorlevel% neq 0 (
  echo BUILD FAILED
  exit /b 1
)
echo BUILD OK: build\minima.exe
