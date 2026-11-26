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

# ---- Native app services: inflate (archive::ZipReader) and SQLite (db::Database) -------------------------
# miniz: one C file (MIT), inflate/deflate + CRC-32 only (archive / stdio / time APIs compiled out).
# SQLite: the public-domain amalgamation, multi-thread mode (each connection is guarded by db::Database).
# Both are plain C sources without a CMake project of their own: FetchContent only downloads them.
enable_language(C)
FetchContent_Declare(
    boson_miniz
    URL https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
    URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Database backends of boson::db (same typed API, chosen per Database::open). An app only links what is ON.
option(BOSON_DB_SQLITE "Build the SQLite backend of boson::db" ON)
option(BOSON_DB_ZEPHYR "Build the ZephyrDB backend of boson::db" ON)
FetchContent_Declare(
    boson_sqlite3
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(boson_miniz)

add_library(boson_miniz STATIC "${boson_miniz_SOURCE_DIR}/miniz.c")
target_include_directories(boson_miniz SYSTEM PUBLIC "${boson_miniz_SOURCE_DIR}")
target_compile_definitions(boson_miniz PUBLIC
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)
target_compile_options(boson_miniz PRIVATE $<$<C_COMPILER_ID:MSVC>:/W0>)

if(BOSON_DB_SQLITE)
FetchContent_MakeAvailable(boson_sqlite3)
add_library(boson_sqlite3 STATIC "${boson_sqlite3_SOURCE_DIR}/sqlite3.c")
target_include_directories(boson_sqlite3 SYSTEM PUBLIC "${boson_sqlite3_SOURCE_DIR}")
target_compile_definitions(boson_sqlite3 PRIVATE
    SQLITE_THREADSAFE=2
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_USE_ALLOCA
    SQLITE_WIN32_NO_ANSI)
target_compile_options(boson_sqlite3 PRIVATE $<$<C_COMPILER_ID:MSVC>:/W0>)
endif()

# ZephyrDB (https://github.com/KelpHect/ZephyrDB), pinned. Linked statically through its C ABI
# (zephyr/c_abi.h); its own warnings stay in its own target. EXCLUDE_FROM_ALL: only zephyrdb_core is built
# (not its DLL / server / tests). Local clone: -DFETCHCONTENT_SOURCE_DIR_BOSON_ZEPHYRDB=D:/Projects/ZephyrDB
set(BOSON_ZEPHYR_COMMIT c50eea79fee693df1931c471819016dacf43a09e)
if(BOSON_DB_ZEPHYR)
    set(ZEPHYR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZEPHYR_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(ZEPHYR_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        boson_zephyrdb
        GIT_REPOSITORY https://github.com/KelpHect/ZephyrDB.git
        GIT_TAG ${BOSON_ZEPHYR_COMMIT}
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(boson_zephyrdb)
    # ZephyrDB's own /W4 build reports C4702 (unreachable code) in executor.cpp; keep its noise out of ours.
    target_compile_options(zephyrdb_core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/wd4702>)
endif()

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
    src/app/lifecycle.cpp
    src/native/notification.cpp
    src/native/dialog.cpp
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
    src/crypto/hash.cpp
    src/archive/zip.cpp
    src/fs/install_transaction.cpp
    src/fs/known_folders.cpp
    src/db/database.cpp
    src/db/kv_store.cpp
    $<$<BOOL:${BOSON_DB_SQLITE}>:src/db/sqlite_backend.cpp>
    $<$<BOOL:${BOSON_DB_ZEPHYR}>:src/db/zephyr_backend.cpp>
    src/net/http_client.cpp
    src/net/http_cache.cpp
    src/net/proxy.cpp
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
    PRIVATE
        boson_miniz
        $<$<BOOL:${BOSON_DB_SQLITE}>:boson_sqlite3>
        $<$<BOOL:${BOSON_DB_ZEPHYR}>:zephyrdb_core>
)
target_compile_definitions(boson_core
    PUBLIC
        BOSON_DB_HAS_SQLITE=$<BOOL:${BOSON_DB_SQLITE}>
        BOSON_DB_HAS_ZEPHYR=$<BOOL:${BOSON_DB_ZEPHYR}>
    PRIVATE
        BOSON_ZEPHYR_COMMIT="${BOSON_ZEPHYR_COMMIT}"
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

add_executable(boson_addon_manager_mini
    examples/addon_manager_mini/main.cpp
)
target_link_libraries(boson_addon_manager_mini PRIVATE boson_core ws2_32)
add_test(NAME boson_addon_manager_mini_selftest COMMAND boson_addon_manager_mini --selftest)

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
# boson::db backend benchmark (not a test; run a Release build by hand).
add_executable(boson_db_bench tests/bench_db.cpp)
target_link_libraries(boson_db_bench PRIVATE boson_core)
# Native app services: hashing, ZIP, staged installs, HTTP client (loopback server), boson::db backends.
add_executable(boson_native_services_tests
    tests/test_native_services.cpp
)
target_link_libraries(boson_native_services_tests PRIVATE boson_core ws2_32)
add_test(NAME boson_native_services_tests COMMAND boson_native_services_tests)
set_tests_properties(boson_native_services_tests PROPERTIES TIMEOUT 300)
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
# File drop, binary IPC (shared buffers / base64) and streaming boson://media. Headless unit tests always run;
# the WebView2 part (including the throughput benchmark) skips when no runtime is installed.
add_executable(boson_transfer_tests
    tests/test_transfer.cpp
)
target_link_libraries(boson_transfer_tests PRIVATE boson_core)
add_test(NAME boson_transfer_tests COMMAND boson_transfer_tests)
set_tests_properties(boson_transfer_tests PROPERTIES TIMEOUT 300)
