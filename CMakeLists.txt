# Minima — cross-platform build.
# Copyright (c) 2026 g-halcyon (https://github.com/g-halcyon)
# Licensed under PolyForm Noncommercial License 1.0 (PolyForm-Noncommercial-1.0).
#
# The Windows target here mirrors build.bat exactly (same flags, same static WebView2
# loader) and is the maintained desktop path. macOS/Linux/Android shells are being
# brought up per PORTING.md; their targets are declared but gated until the core is
# extracted behind src/platform/platform.h.
#
#   cmake -B build -S .
#   cmake --build build --config Release
#
# On Windows you can still use the dependency-free build.bat instead of CMake.
cmake_minimum_required(VERSION 3.20)
project(minima LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Portable core (grows as main.cpp is split per roadmap step 1). Already extracted and
# shared by every shell as header-only modules under src/core/:
#   src/core/ui_assets.h  — all screens (chrome, start, history, AI, wizard, sidebar)
#   src/core/json.h       — JSON reader/escaper
#   src/core/models.h     — Bookmark/History/Settings + search-engine table
#   src/core/adblock.h    — ad/tracker host list + suffix matcher
#   src/core/storage.h    — (de)serialization of the data files
#   src/core/ai_models.h  — on-device AI model catalog + lookup
#   src/core/catalog.h    — curated extension catalog + GitHub asset-URL parser
# The core is header-only today; .cpp modules appear here once the Tab model and the
# platform.h-backed logic are split out of the shell.
set(CORE_SOURCES
)

if(WIN32)
    # --- Windows shell: Win32 + WebView2 (currently the whole app, in main.cpp) -------
    add_executable(minima WIN32 src/main.cpp ${CORE_SOURCES} src/app.rc)
    target_include_directories(minima PRIVATE
        src
        vendor/webview2-pkg/build/native/include)
    target_compile_definitions(minima PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(minima PRIVATE /utf-8 /EHsc /W3 /O2 /MT)
    endif()
    target_link_libraries(minima PRIVATE
        "${CMAKE_SOURCE_DIR}/vendor/webview2-pkg/build/native/x64/WebView2LoaderStatic.lib"
        user32 gdi32 ole32 shell32 advapi32 version winhttp ws2_32 dwmapi)

elseif(APPLE)
    # --- macOS shell: NSWindow + WKWebView (Objective-C++) ----------------------------
    # Core browsing works (tabs, chrome UI, adblock, session); extensions are not
    # possible on WKWebView and the AI sidecar is still TODO — see PORTING.md.
    enable_language(OBJCXX)
    add_executable(minima MACOSX_BUNDLE src/platform/macos/shell.mm ${CORE_SOURCES})
    target_include_directories(minima PRIVATE src)
    set_source_files_properties(src/platform/macos/shell.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(minima PRIVATE "-framework Cocoa" "-framework WebKit")

elseif(UNIX)
    # --- Linux shell: GTK3 + WebKitGTK ------------------------------------------------
    # apt install build-essential pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev
    # Core browsing works (tabs, chrome UI, adblock, session, find); extensions are not
    # possible on WebKitGTK and the AI sidecar is still TODO — see PORTING.md.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKIT REQUIRED gtk+-3.0 webkit2gtk-4.1)
    add_executable(minima src/platform/linux/shell.cpp ${CORE_SOURCES})
    target_include_directories(minima PRIVATE src ${WEBKIT_INCLUDE_DIRS})
    target_link_libraries(minima PRIVATE ${WEBKIT_LIBRARIES})
    target_compile_options(minima PRIVATE ${WEBKIT_CFLAGS_OTHER})
endif()

# Android is built through Gradle + the NDK (see android/), which invokes this file's
# CORE_SOURCES as a shared library; it is not configured from a desktop cmake run.
