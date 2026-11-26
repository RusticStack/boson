cmake_minimum_required(VERSION 3.28)

project(Boson LANGUAGES CXX)

# CMake 4.4 does not yet map CXX26 for MSVC; /std:c++latest is passed explicitly below.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC's default Release flags carry /Ob2 and boson_core asks for /Ob3; both on one command line is warning D9025.
string(REPLACE "/Ob2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

# Static CRT (/MT): executables no longer need the Visual C++ Redistributable (VCRUNTIME140.dll, MSVCP140.dll),
# at the cost of a larger .exe. See docs/BENCHMARKS.md for both sizes.
option(BOSON_STATIC_CRT "Link the MSVC runtime statically (/MT, /MTd)" OFF)
if(BOSON_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT BOSON_IPO_SUPPORTED OUTPUT BOSON_IPO_ERROR)
if(BOSON_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
else()
    message(STATUS "IPO/LTO not supported: ${BOSON_IPO_ERROR}")
endif()

include(cmake/FetchWebView2.cmake)
include(cmake/EmbedAssets.cmake)

include(FetchContent)

FetchContent_Declare(
    glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG v9.1.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glaze)

set(STDEXEC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STDEXEC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    stdexec
    GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
    GIT_TAG nvhpc-26.05
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(stdexec)

add_library(boson_core STATIC
    src/core/window.cpp
    src/core/webview.cpp
    src/core/webview_events.cpp
    src/core/environment.cpp
    src/core/composition.cpp
    src/core/ui_timer.cpp
    src/core/webview_host.cpp
    src/core/protocol.cpp
    src/exec/task_runner.cpp
    src/ipc/bridge.cpp
    src/ipc/sync_bridge.cpp
    src/ipc/sync_boot.cpp
    src/app/lifecycle.cpp
    src/core/window_manager.cpp
    src/native/notification.cpp
    src/native/dialog.cpp
    src/native/message_box.cpp
    src/native/permissions.cpp
    src/native/web_notifications.cpp
    src/native/browser_args.cpp
    src/native/metrics.cpp
    src/native/system_info.cpp
    src/native/session.cpp
    src/shell/app_id.cpp
    src/shell/icon.cpp
    src/shell/taskbar.cpp
    src/shell/jump_list.cpp
    src/shell/toast.cpp
    src/shell/toast_activation.cpp
    src/shell/power.cpp
    src/shell/auto_launch.cpp
    src/compat/electron_ipc.cpp
    src/webview/print.cpp
    src/webview/capture.cpp
    src/webview/find.cpp
    src/webview/cookies.cpp
    src/webview/profile.cpp
    src/webview/proxy.cpp
    src/webview/security_events.cpp
    src/webview/drm.cpp
    src/desktop/hidden_window.cpp
    src/desktop/clipboard.cpp
    src/desktop/shell.cpp
    src/desktop/shortcuts.cpp
    src/desktop/deep_link.cpp
    src/desktop/screen.cpp
    src/desktop/window_state.cpp
    src/security/common.cpp
    src/security/origin.cpp
    src/security/capabilities.cpp
    src/security/web_security.cpp
    src/security/safe_storage.cpp
    src/security/credential_store.cpp
    src/security/fs_scope.cpp
    src/security/scoped_fs.cpp
    src/store/store.cpp
    src/titlebar/theme.cpp
    src/titlebar/window_chrome.cpp
    src/titlebar/caption_accessibility.cpp
    src/titlebar/caption_buttons.cpp
    src/titlebar/titlebar.cpp
    src/native/tray.cpp
    src/menu/accelerator.cpp
    src/menu/menu.cpp
    src/menu/menu_bar.cpp
    src/shipping/logger.cpp
    src/shipping/version.cpp
    src/shipping/crypto.cpp
    src/shipping/http.cpp
    src/shipping/authenticode.cpp
    src/shipping/updater.cpp
    src/shipping/webview2_runtime.cpp
    src/shipping/crash_reporter.cpp
    src/transfer/base64.cpp
    src/transfer/binary_transport.cpp
    src/transfer/file_drop.cpp
    src/transfer/stream_server.cpp
    src/transfer/drag_out.cpp
)

# Windows 10+ compatibility + Per-Monitor V2 manifest for every executable linking boson_core.
target_sources(boson_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/boson.manifest")

target_include_directories(boson_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated"
)

target_compile_definitions(boson_core
    PUBLIC
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
)

target_compile_options(boson_core
    PUBLIC
        $<$<CXX_COMPILER_ID:MSVC>:/std:c++latest;/Zc:__cplusplus;/permissive-;/EHsc;/bigobj;/utf-8;/W4>
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/O2;/Ob3;/DNDEBUG>
)

target_link_options(boson_core
    PUBLIC
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/OPT:REF;/OPT:ICF;/DEBUG:NONE;/INCREMENTAL:NO>
)

target_link_libraries(boson_core
    PUBLIC
        glaze::glaze
        STDEXEC::stdexec
        WebView2::Static
        version
        advapi32
        ole32
        oleaut32
        shlwapi
        dwmapi
        dcomp
        user32
        gdi32
        shell32
        propsys
        runtimeobject
        wtsapi32
        shcore
        windowscodecs
        crypt32
        uiautomationcore
        bcrypt
        winhttp
        wintrust
        dbghelp
)

add_executable(boson_demo WIN32
    examples/demo/main.cpp
)
target_link_libraries(boson_demo PRIVATE boson_core)

# Benchmark assets shared verbatim with bench/wails_browser (boson_browser --bench).
set(BOSON_BENCH_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bench/shared")
file(READ "${BOSON_BENCH_SHARED_DIR}/bench_shell.js" BOSON_BENCH_SHELL_JS)
file(READ "${BOSON_BENCH_SHARED_DIR}/bench_tab.html" BOSON_BENCH_TAB_HTML)
configure_file(examples/browser/bench_assets.hpp.in "${CMAKE_BINARY_DIR}/generated/browser_bench_assets.hpp" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${BOSON_BENCH_SHARED_DIR}/bench_shell.js" "${BOSON_BENCH_SHARED_DIR}/bench_tab.html")

add_executable(boson_browser WIN32
    examples/browser/main.cpp
)
target_link_libraries(boson_browser PRIVATE boson_core)

# Multi-window demo (app::WindowManager): pop-out dashboard, modal settings, emit by label. --smoke runs it unattended.
add_executable(boson_windows_demo WIN32
    examples/windows/main.cpp
)
target_link_libraries(boson_windows_demo PRIVATE boson_core)

# Native platform pieces end to end: dialogs, permissions + JS prompt hook, web notifications -> toasts, zoom,
# metrics, locale / preferences, shutdown blocking, close-to-tray (docs/NATIVE.md).
add_executable(boson_native_example WIN32
    examples/native/main.cpp
)
target_link_libraries(boson_native_example PRIVATE boson_core)

add_executable(boson_bench_app WIN32
    examples/bench_app/main.cpp
)
target_link_libraries(boson_bench_app PRIVATE boson_core)

# Process-tree benchmark harness (any framework's executable).
add_executable(boson_bench
    tools/bench/main.cpp
)
target_link_libraries(boson_bench PRIVATE boson_core)

add_executable(boson_tsgen
    tools/tsgen/main.cpp
)
target_link_libraries(boson_tsgen PRIVATE boson_core)

# Hardware-DRM verification spike (issue #15): plays public PlayReady test streams in windowed and composition
# hosting and records playback / capture evidence. Manual tool: needs network, a GPU and a real display.
add_executable(boson_drm_spike
    tools/drm_spike/main.cpp
)
target_link_libraries(boson_drm_spike PRIVATE boson_core d3d11 dxgi dwmapi runtimeobject)

# ECDSA P-256 key generation / update signing for boson::shipping::Updater.
add_executable(boson_updater_sign
    tools/updater-sign/main.cpp
)
target_link_libraries(boson_updater_sign PRIVATE boson_core)

# ---- Shipping: install rules, ZIP / MSI (CPack) and MSIX packaging, optional signing -------------------
include(cmake/BosonPackaging.cmake)
option(BOSON_PACKAGE_DEMO "Package boson_demo with boson_package() (cpack / boson_demo_msix)" ON)
if(BOSON_PACKAGE_DEMO)
    boson_package(boson_demo
        NAME "Boson Demo"
        VERSION 0.1.0
        VENDOR "Boson"
        DESCRIPTION "Boson C++26 WebView2 demo"
        APP_USER_MODEL_ID "Boson.Demo"
        UPGRADE_GUID "6E0F3B9C-5B1E-4C5A-9E7B-1C2D3E4F5A60"
        PROTOCOLS boson-demo
        LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        CLEAN_APPDATA_DIR "Boson Demo"
        MSIX_IDENTITY "Boson.Demo"
        MSIX_PUBLISHER "CN=Boson Demo Publisher")
    boson_sign_target(boson_demo boson_updater_sign)
endif()

enable_testing()
add_executable(boson_tests
    tests/test_boson.cpp
)
target_link_libraries(boson_tests PRIVATE boson_core)
add_test(NAME boson_tests COMMAND boson_tests)

# Drives the installed WebView2 runtime (skips when none is installed). Opens two small windows.
add_executable(boson_integration_tests
    tests/test_integration.cpp
)
target_link_libraries(boson_integration_tests PRIVATE boson_core)
add_test(NAME boson_integration_tests COMMAND boson_integration_tests)
set_tests_properties(boson_integration_tests PROPERTIES LABELS integration TIMEOUT 180)

# Taskbar, jump list, toasts, power and auto-launch. Opens one small window (SW_SHOWNOACTIVATE) and shows
# silent toasts; every registry value / shortcut it creates is unique per run and removed before exit.
add_executable(boson_shell_integration_tests
    tests/test_shell_integration.cpp
)
target_link_libraries(boson_shell_integration_tests PRIVATE boson_core)
add_test(NAME boson_shell_integration_tests COMMAND boson_shell_integration_tests)
set_tests_properties(boson_shell_integration_tests PROPERTIES LABELS integration TIMEOUT 180)
add_executable(boson_porting_tests
    tests/test_porting.cpp
)
target_include_directories(boson_porting_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tools/bench")
target_link_libraries(boson_porting_tests PRIVATE boson_core)
add_test(NAME boson_porting_tests COMMAND boson_porting_tests)
# Bundles the TypeScript compat-shim self-test (tests/js/compat_selftest.ts) when bun is available; the test
# skips that part otherwise.
find_program(BOSON_BUN_EXECUTABLE bun)
if(BOSON_BUN_EXECUTABLE)
    set(BOSON_PORTING_SHIM_JS "${CMAKE_BINARY_DIR}/porting_shims_selftest.js")
    add_custom_command(
        OUTPUT "${BOSON_PORTING_SHIM_JS}"
        COMMAND "${BOSON_BUN_EXECUTABLE}" build "${CMAKE_CURRENT_SOURCE_DIR}/tests/js/compat_selftest.ts"
                --format=iife "--outfile=${BOSON_PORTING_SHIM_JS}"
        DEPENDS
            tests/js/compat_selftest.ts
            tests/js/tsconfig.json
            tests/js/fixtures/wails/bindings/events/index.ts
            tests/js/fixtures/wails/bindings/github.com/wailsapp/wails/v3/internal/eventcreate.ts
            tests/js/fixtures/wails/bindings/wailsfixture/index.ts
            tests/js/fixtures/wails/bindings/wailsfixture/models.ts
            tests/js/fixtures/wails/bindings/wailsfixture/tabservice.ts
            frontend/src/boson.ts
            frontend/src/generated/boson-method-params.ts
            frontend/src/compat/electron.ts
            frontend/src/dragout.ts
            frontend/src/native.ts
            frontend/src/compat/tauri.ts
            frontend/src/compat/wails.ts
        COMMENT "Bundling the compat shim self-test"
        VERBATIM)
    add_custom_target(boson_porting_shim_js DEPENDS "${BOSON_PORTING_SHIM_JS}")
    add_dependencies(boson_porting_tests boson_porting_shim_js)
    target_compile_definitions(boson_porting_tests PRIVATE BOSON_PORTING_SHIM_JS="${BOSON_PORTING_SHIM_JS}")
    # tsc --noEmit over the self-test and the real wails3 bindings fixture (strict, the @wailsio/runtime alias
    # from tests/js/tsconfig.json). Needs frontend/node_modules (cd frontend && bun install).
    set(BOSON_FRONTEND_TSC "${CMAKE_CURRENT_SOURCE_DIR}/frontend/node_modules/typescript/bin/tsc")
    add_test(NAME boson_compat_typecheck
        COMMAND "${BOSON_BUN_EXECUTABLE}" "${BOSON_FRONTEND_TSC}" -p "${CMAKE_CURRENT_SOURCE_DIR}/tests/js/tsconfig.json")
    if(NOT EXISTS "${BOSON_FRONTEND_TSC}")
        set_tests_properties(boson_compat_typecheck PROPERTIES DISABLED TRUE)
    endif()
endif()
# WebView features (print/PDF, capture, find, cookies, profiles, proxy, auth events). Unit checks always run;
# the runtime part skips when no WebView2 runtime is installed. Opens one small non-activated window.
add_executable(boson_webview_features_tests
    tests/test_webview_features.cpp
)
target_link_libraries(boson_webview_features_tests PRIVATE boson_core ws2_32)
add_test(NAME boson_webview_features_tests COMMAND boson_webview_features_tests)
set_tests_properties(boson_webview_features_tests PROPERTIES LABELS integration TIMEOUT 240)
# Desktop basics: clipboard, shell, global shortcuts, deep links, screen, window state.
add_executable(boson_desktop_tests
    tests/test_desktop.cpp
)
target_link_libraries(boson_desktop_tests PRIVATE boson_core)
add_test(NAME boson_desktop_tests COMMAND boson_desktop_tests)
set_tests_properties(boson_desktop_tests PROPERTIES TIMEOUT 180)
# Security & storage: capability policy, CSP headers (real WebView, skips without runtime), DPAPI,
# Credential Manager (unique test targets, removed at the end) and the scoped file system.
add_executable(boson_security_tests
    tests/test_security.cpp
)
target_link_libraries(boson_security_tests PRIVATE boson_core)
add_test(NAME boson_security_tests COMMAND boson_security_tests)
set_tests_properties(boson_security_tests PROPERTIES TIMEOUT 180)
# Headless titlebar: hit testing, overlay geometry/DPI/RTL, UI Automation, theme; WebView2 part skips without a runtime.
add_executable(boson_titlebar_tests
    tests/test_titlebar.cpp
)
target_link_libraries(boson_titlebar_tests PRIVATE boson_core)
add_test(NAME boson_titlebar_tests COMMAND boson_titlebar_tests)
set_tests_properties(boson_titlebar_tests PROPERTIES TIMEOUT 180)
# Menus, accelerators and tray. Opens small SW_SHOWNOACTIVATE windows; the WebView part skips without a runtime.
add_executable(boson_menu_tests
    tests/test_menu.cpp
)
target_link_libraries(boson_menu_tests PRIVATE boson_core)
add_test(NAME boson_menu_tests COMMAND boson_menu_tests)
set_tests_properties(boson_menu_tests PROPERTIES TIMEOUT 180)
# Window manager: windows by label, owned/modal windows, close veto, state per label, Window controller and
# emit_to(label). Opens small SW_SHOWNOACTIVATE windows; the WebView2 part skips without a runtime.
add_executable(boson_window_tests
    tests/test_windows.cpp
)
target_link_libraries(boson_window_tests PRIVATE boson_core)
add_test(NAME boson_window_tests COMMAND boson_window_tests)
set_tests_properties(boson_window_tests PROPERTIES LABELS integration TIMEOUT 240)
if(BOSON_BUN_EXECUTABLE)
    # windows.ts + the Tauri / Electron / Wails window shims, run in a real window against the Window controller.
    set(BOSON_WINDOWS_SELFTEST_JS "${CMAKE_BINARY_DIR}/windows_selftest.js")
    add_custom_command(
        OUTPUT "${BOSON_WINDOWS_SELFTEST_JS}"
        COMMAND "${BOSON_BUN_EXECUTABLE}" build "${CMAKE_CURRENT_SOURCE_DIR}/tests/js/windows_selftest.ts"
                --format=iife "--outfile=${BOSON_WINDOWS_SELFTEST_JS}"
        DEPENDS
            tests/js/windows_selftest.ts
            frontend/src/boson.ts
            frontend/src/windows.ts
            frontend/src/generated/boson-method-params.ts
            frontend/src/compat/electron.ts
            frontend/src/compat/tauri.ts
            frontend/src/compat/wails.ts
        COMMENT "Bundling the window-manager shim self-test"
        VERBATIM)
    add_custom_target(boson_windows_selftest_js DEPENDS "${BOSON_WINDOWS_SELFTEST_JS}")
    add_dependencies(boson_window_tests boson_windows_selftest_js)
    target_compile_definitions(boson_window_tests PRIVATE BOSON_WINDOWS_SELFTEST_JS="${BOSON_WINDOWS_SELFTEST_JS}")
endif()
# Shipping: versions, crypto, manifests, logger, loopback downloads, update helper, crash dumps.
add_executable(boson_shipping_tests
    tests/test_shipping.cpp
)
target_link_libraries(boson_shipping_tests PRIVATE boson_core ws2_32)
add_test(NAME boson_shipping_tests COMMAND boson_shipping_tests)
set_tests_properties(boson_shipping_tests PROPERTIES TIMEOUT 300)

# Authenticode signing with a throw-away self-signed certificate (created in and removed from CurrentUser\My).
if(BOSON_SIGNTOOL AND BOSON_PACKAGE_DEMO)
    add_test(NAME boson_signing_test
        COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_signing.ps1"
            -CMake "${CMAKE_COMMAND}"
            -SignScript "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BosonSign.cmake"
            -SignTool "${BOSON_SIGNTOOL}"
            -Exe "$<TARGET_FILE:boson_updater_sign>"
            -MakeAppx "${BOSON_MAKEAPPX}"
            -Manifest "${CMAKE_BINARY_DIR}/boson_package/boson_demo/msix/AppxManifest.xml"
            -AssetScript "${CMAKE_CURRENT_SOURCE_DIR}/cmake/msix/generate_assets.ps1")
    set_tests_properties(boson_signing_test PROPERTIES TIMEOUT 120 LABELS signing)
endif()
# Persistent settings store: missing / corrupt files, concurrent updates, a crash during save (child
# process), the Store IPC controller and "store:changed" events. Headless; everything lives in a temp folder.
add_executable(boson_store_tests
    tests/test_store.cpp
)
target_link_libraries(boson_store_tests PRIVATE boson_core)
add_test(NAME boson_store_tests COMMAND boson_store_tests)
set_tests_properties(boson_store_tests PROPERTIES TIMEOUT 120)
# Synchronous IPC (SyncBridge host object, BootValues, ElectronIpc.sendSync) and native drag-out data objects.
# Headless checks always run; the WebView2 part (latency numbers) skips without a runtime. Opens one small
# non-activated window; never performs a real mouse drag.
add_executable(boson_sync_tests
    tests/test_sync.cpp
)
target_link_libraries(boson_sync_tests PRIVATE boson_core)
add_test(NAME boson_sync_tests COMMAND boson_sync_tests)
set_tests_properties(boson_sync_tests PROPERTIES TIMEOUT 240)

# Native platform pieces: dialogs (auto-driven, never left open), message boxes, permissions, web notifications,
# zoom, browser arguments, metrics, locale / preferences, session end, close-to-tray. Opens one small
# non-activated window; the WebView2 part skips when no runtime is installed.
add_executable(boson_native_tests
    tests/test_native.cpp
)
target_link_libraries(boson_native_tests PRIVATE boson_core)
add_test(NAME boson_native_tests COMMAND boson_native_tests)
set_tests_properties(boson_native_tests PROPERTIES LABELS integration TIMEOUT 240)
# File drop, binary IPC (shared buffers / base64) and streaming boson://media. Headless unit tests always run;
# the WebView2 part (including the throughput benchmark) skips when no runtime is installed.
add_executable(boson_transfer_tests
    tests/test_transfer.cpp
)
target_link_libraries(boson_transfer_tests PRIVATE boson_core)
add_test(NAME boson_transfer_tests COMMAND boson_transfer_tests)
set_tests_properties(boson_transfer_tests PROPERTIES TIMEOUT 300)
