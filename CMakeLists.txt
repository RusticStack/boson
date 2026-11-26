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
# One self-contained .exe (docs/SHIPPING.md "Single executable"): static CRT for the whole tree, plus the
# boson_single_exe(<target>) helper that checks the result imports only Windows system DLLs.
option(BOSON_SINGLE_EXE "Build self-contained single executables (implies BOSON_STATIC_CRT)" OFF)
if(BOSON_SINGLE_EXE)
    set(BOSON_STATIC_CRT ON CACHE BOOL "" FORCE)
endif()
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
include(cmake/BosonSingleExe.cmake)
include(cmake/BosonPgo.cmake)

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

# Extra Release switches for experiments (docs/PERFORMANCE.md "Build configuration"). Empty by default: /Gw,
# /Gy and /Zc:inline produced byte-identical executables under /GL + /OPT:REF,ICF, so they are not set.
set(BOSON_RELEASE_CODEGEN_FLAGS "" CACHE STRING "Extra MSVC Release compile flags for Boson targets, e.g. /Os")
# RTTI off in Release: nothing in Boson uses dynamic_cast / typeid (exceptions do not need RTTI). -12 to -17 % exe size.
# The flag is PUBLIC (an app that links boson_core and uses dynamic_cast / typeid must set -DBOSON_NO_RTTI=OFF).
option(BOSON_NO_RTTI "Release builds: /GR- (smaller executables; no dynamic_cast / typeid)" ON)
if(BOSON_NO_RTTI)
    list(APPEND BOSON_RELEASE_CODEGEN_FLAGS "/GR-")
endif()
option(BOSON_AVX2 "Compile with /arch:AVX2 (needs a 2013+ CPU; the default stays baseline x64)" OFF)

add_library(boson_core STATIC
    src/core/trace.cpp
    src/core/window.cpp
    src/core/webview.cpp
    src/core/webview_events.cpp
    src/core/environment.cpp
    src/core/composition.cpp
    src/core/ui_timer.cpp
    src/core/text_convert.cpp
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
    src/webview/cdp.cpp
    src/webview/cdp_server.cpp
    src/webview/guest.cpp
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
    src/process/command_line.cpp
    src/process/detail.cpp
    src/process/process.cpp
    src/process/pipe_ipc.cpp
    src/process/policy.cpp
    src/process/sidecar.cpp
    src/terminal/pty.cpp
    src/terminal/output_channel.cpp
    src/terminal/terminal_host.cpp
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
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/O2;/Ob3;/DNDEBUG;${BOSON_RELEASE_CODEGEN_FLAGS}>
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<BOOL:${BOSON_AVX2}>>:/arch:AVX2>
)

target_link_options(boson_core
    PUBLIC
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/OPT:REF;/OPT:ICF;/INCREMENTAL:NO>
        # /GENPROFILE only writes its .pgd when a PDB is produced (verified), so PGO builds link with /DEBUG.
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>,$<STREQUAL:${BOSON_PGO},OFF>>:/DEBUG:NONE>
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>,$<NOT:$<STREQUAL:${BOSON_PGO},OFF>>>:/DEBUG>
)

# Delay-load system DLLs that only some modules use, so an app that does not call them never maps them:
# every DLL costs a LoadLibrary at startup plus its (and its dependencies') private data. The delay-load helper
# (delayimp.lib) resolves the import on the first call. Consumers inherit the flags (PUBLIC link options).
#  - Not delay-loaded on purpose: dbghelp (the crash reporter must not load a DLL from inside an unhandled
#    exception filter, so it stays an ordinary import) and everything user32 / gdi32 / ole32 / shell32 / advapi32 /
#    dwmapi / shcore / shlwapi / version, which the window and the WebView2 loader map before main anyway.
#  - /IGNORE:4199: not every executable that links boson_core ends up importing every listed DLL (unused objects
#    of the static library are dropped), and an unused /DELAYLOAD entry is warning LNK4199.
option(BOSON_DELAYLOAD "Delay-load rarely used system DLLs (uiautomationcore, dcomp, winhttp, crypt32, ...)" ON)
set(BOSON_DELAYLOAD_DLLS
    uiautomationcore.dll dcomp.dll windowscodecs.dll crypt32.dll bcrypt.dll winhttp.dll wintrust.dll
    wtsapi32.dll ws2_32.dll runtimeobject.dll propsys.dll)
if(MSVC AND BOSON_DELAYLOAD)
    set(_boson_delay_flags "/IGNORE:4199")
    foreach(_dll IN LISTS BOSON_DELAYLOAD_DLLS)
        list(APPEND _boson_delay_flags "/DELAYLOAD:${_dll}")
    endforeach()
    target_link_options(boson_core PUBLIC ${_boson_delay_flags})
    target_link_libraries(boson_core PUBLIC delayimp)
    target_compile_definitions(boson_core PUBLIC BOSON_DELAYLOAD_ENABLED=1)
endif()

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
        ws2_32
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

# ---- boson_gpu: native GPU surfaces composited with WebView2 (docs/GPU.md) ----------------------------------
# A separate library so apps that do not render natively never load d3d11 / d3d12 / dwrite.
find_program(BOSON_FXC fxc
    HINTS "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkBinPath}/x64"
    PATHS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" "C:/Program Files (x86)/Windows Kits/10/bin/x64"
    REQUIRED)
set(BOSON_GPU_SHADER_DIR "${CMAKE_BINARY_DIR}/generated/gpu")
file(MAKE_DIRECTORY "${BOSON_GPU_SHADER_DIR}")
set(BOSON_GPU_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/batch2d.hlsl")
add_custom_command(
    OUTPUT "${BOSON_GPU_SHADER_DIR}/batch2d_vs.h" "${BOSON_GPU_SHADER_DIR}/batch2d_ps.h"
    COMMAND "${BOSON_FXC}" /nologo /O3 /WX /T vs_4_0 /E vs_main /Vn g_batch2d_vs /Fh "${BOSON_GPU_SHADER_DIR}/batch2d_vs.h" "${BOSON_GPU_SHADER_SRC}"
    COMMAND "${BOSON_FXC}" /nologo /O3 /WX /T ps_4_0 /E ps_main /Vn g_batch2d_ps /Fh "${BOSON_GPU_SHADER_DIR}/batch2d_ps.h" "${BOSON_GPU_SHADER_SRC}"
    DEPENDS "${BOSON_GPU_SHADER_SRC}"
    COMMENT "Compiling gpu::Renderer2D shaders (fxc)"
    VERBATIM)

# Vulkan headers only (vulkan-1.dll is loaded at runtime): the installed SDK when present, else Vulkan-Headers.
file(GLOB _boson_vulkan_sdk_includes "C:/VulkanSDK/*/Include")
find_path(BOSON_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/Include" PATHS ${_boson_vulkan_sdk_includes})
if(NOT BOSON_VULKAN_INCLUDE_DIR)
    FetchContent_Declare(
        vulkan_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.4.309
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(vulkan_headers)
    set(BOSON_VULKAN_INCLUDE_DIR "${vulkan_headers_SOURCE_DIR}/include")
endif()
message(STATUS "Boson GPU: Vulkan headers from ${BOSON_VULKAN_INCLUDE_DIR}")

add_library(boson_gpu STATIC
    src/gpu/device.cpp
    src/gpu/render_loop.cpp
    src/gpu/surface.cpp
    src/gpu/renderer2d.cpp
    src/gpu/vulkan_interop.cpp
    "${BOSON_GPU_SHADER_DIR}/batch2d_vs.h"
    "${BOSON_GPU_SHADER_DIR}/batch2d_ps.h"
)
target_include_directories(boson_gpu
    PUBLIC "${BOSON_VULKAN_INCLUDE_DIR}"
    PRIVATE "${BOSON_GPU_SHADER_DIR}")
target_link_libraries(boson_gpu PUBLIC boson_core d3d11 d3d12 dxgi dwrite dcomp)

add_executable(boson_gpu_demo WIN32
    examples/gpu_demo/main.cpp
    examples/gpu_demo/demo_page.hpp
)
target_link_libraries(boson_gpu_demo PRIVATE boson_gpu)

# The smallest app: reference for the single-executable build.
option(BOSON_HELLO_EMBED_BOOTSTRAPPER "Compile the WebView2 Evergreen bootstrapper into boson_hello (single-exe only)" OFF)
add_executable(boson_hello WIN32
    examples/hello/main.cpp
)
target_link_libraries(boson_hello PRIVATE boson_core)

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

# boson::process from C++: exec, streaming, a supervised Node sidecar (console).
add_executable(boson_process_example
    examples/process/main.cpp
)
target_link_libraries(boson_process_example PRIVATE boson_core)

add_executable(boson_addon_manager_mini
    examples/addon_manager_mini/main.cpp
)
target_link_libraries(boson_addon_manager_mini PRIVATE boson_core ws2_32)
add_test(NAME boson_addon_manager_mini_selftest COMMAND boson_addon_manager_mini --selftest)

add_executable(boson_bench_app WIN32
    examples/bench_app/main.cpp
)
target_link_libraries(boson_bench_app PRIVATE boson_core)
boson_enable_pgo(boson_bench_app)

# Every module constructed and registered: measures the idle cost of an app that uses all of Boson
# (docs/PERFORMANCE.md, "Memory footprint, threads and idle cost").
add_executable(boson_kitchen_sink WIN32
    examples/kitchen_sink/main.cpp
)
target_link_libraries(boson_kitchen_sink PRIVATE boson_core)

# Process-tree benchmark harness (any framework's executable).
add_executable(boson_bench
    tools/bench/main.cpp
)
target_link_libraries(boson_bench PRIVATE boson_core)

add_executable(boson_tsgen
    tools/tsgen/main.cpp
)
target_link_libraries(boson_tsgen PRIVATE boson_core boson_gpu)

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
# <boson-webview> element, guests (partitions, preload, popups, downloads, IPC) and the CDP endpoint. The element
# part needs the bundled frontend/src/webview_element.ts (bun); BOSON_PLAYWRIGHT_DIR enables the Playwright check.
add_executable(boson_webview_element_tests
    tests/test_webview_element.cpp
)
target_link_libraries(boson_webview_element_tests PRIVATE boson_core ws2_32)
target_compile_definitions(boson_webview_element_tests PRIVATE BOSON_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(BOSON_BUN_EXECUTABLE)
    # frontend/src/webview_element.ts as one IIFE script (registers <boson-webview>), for the test and the example.
    set(BOSON_ELEMENT_JS "${CMAKE_BINARY_DIR}/generated/webview_element.js")
    add_custom_command(
        OUTPUT "${BOSON_ELEMENT_JS}"
        COMMAND "${BOSON_BUN_EXECUTABLE}" build "${CMAKE_CURRENT_SOURCE_DIR}/frontend/src/webview_element.ts"
                --format=iife --minify "--outfile=${BOSON_ELEMENT_JS}"
        DEPENDS frontend/src/webview_element.ts frontend/src/boson.ts frontend/src/generated/boson-bindings.d.ts
        COMMENT "Bundling <boson-webview>"
        VERBATIM)
    # Same bytes as a header for compilers without #embed (examples/webview_element/main.cpp picks one).
    set(BOSON_ELEMENT_JS_HEADER "${CMAKE_BINARY_DIR}/generated/webview_element_js.hpp")
    add_custom_command(
        OUTPUT "${BOSON_ELEMENT_JS_HEADER}"
        COMMAND "${CMAKE_COMMAND}" "-DIN=${BOSON_ELEMENT_JS}" "-DOUT=${BOSON_ELEMENT_JS_HEADER}" -DNAME=kElementJs
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
        DEPENDS "${BOSON_ELEMENT_JS}" cmake/EmbedFile.cmake
        VERBATIM)
    add_custom_target(boson_webview_element_js DEPENDS "${BOSON_ELEMENT_JS}" "${BOSON_ELEMENT_JS_HEADER}")
    add_dependencies(boson_webview_element_tests boson_webview_element_js)
    target_compile_definitions(boson_webview_element_tests PRIVATE BOSON_ELEMENT_JS="${BOSON_ELEMENT_JS}")
    # Example: split view of two <boson-webview> panes, popups, --cdp endpoint. The bundle is #embed-ded.
    add_executable(boson_webview_element WIN32
        examples/webview_element/main.cpp
    )
    target_link_libraries(boson_webview_element PRIVATE boson_core)
    target_include_directories(boson_webview_element PRIVATE "${CMAKE_BINARY_DIR}/generated")
    add_dependencies(boson_webview_element boson_webview_element_js)
    set_source_files_properties(examples/webview_element/main.cpp PROPERTIES OBJECT_DEPENDS "${BOSON_ELEMENT_JS_HEADER}")
endif()
add_test(NAME boson_webview_element_tests COMMAND boson_webview_element_tests)
set_tests_properties(boson_webview_element_tests PROPERTIES LABELS integration TIMEOUT 400)
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
# Lifecycle: exit hooks, lazy scheduler pool, quit() without waiting for the browser process, WebView::discard()
# and SleepPolicy::discard. Opens one small SW_SHOWNOACTIVATE window; the WebView2 part skips without a runtime.
add_executable(boson_lifecycle_tests
    tests/test_lifecycle.cpp
)
target_link_libraries(boson_lifecycle_tests PRIVATE boson_core)
add_test(NAME boson_lifecycle_tests COMMAND boson_lifecycle_tests)
set_tests_properties(boson_lifecycle_tests PROPERTIES LABELS integration TIMEOUT 240)
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

# boson::db backend benchmark (not a test; run a Release build by hand).
add_executable(boson_db_bench tests/bench_db.cpp)
target_link_libraries(boson_db_bench PRIVATE boson_core)
# IPC hot-path tests: conversions, Router table, envelopes, UiMarshal coalescing, Bridge event batching.
add_executable(boson_ipc_hotpath_tests tests/test_ipc_hotpath.cpp)
target_link_libraries(boson_ipc_hotpath_tests PRIVATE boson_core)
add_test(NAME boson_ipc_hotpath_tests COMMAND boson_ipc_hotpath_tests)
set_tests_properties(boson_ipc_hotpath_tests PROPERTIES TIMEOUT 240)
# IPC hot-path micro-benchmark: allocations / bytes / wake-ups / ns per call (not a test; run a Release build by hand).
add_executable(boson_ipc_bench tests/bench_ipc.cpp)
target_link_libraries(boson_ipc_bench PRIVATE boson_core)
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
# Child processes: argv quoting (incl. cmd.exe), pipes, extra handles, jobs (kill tree / kill-on-close), exec,
# sidecars, named pipes, Node IPC (when node.exe is on PATH) and the Process controller through WebView2 (skips
# without a runtime). The test executable is also every child process (--child modes).
add_executable(boson_process_tests
    tests/test_process.cpp
)
target_link_libraries(boson_process_tests PRIVATE boson_core)
add_test(NAME boson_process_tests COMMAND boson_process_tests)
set_tests_properties(boson_process_tests PROPERTIES TIMEOUT 300)
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
# Native GPU surfaces: D3D11 / D3D12 devices, offscreen readback of the 2D batch renderer and DirectWrite text,
# composition swap chains (create / resize / present, idle = no presents), device-lost recovery and Vulkan
# interop readback (skips without vulkan-1.dll). Opens one small SW_SHOWNOACTIVATE window.
add_executable(boson_gpu_tests
    tests/test_gpu.cpp
)
target_link_libraries(boson_gpu_tests PRIVATE boson_gpu)
add_test(NAME boson_gpu_tests COMMAND boson_gpu_tests)
set_tests_properties(boson_gpu_tests PROPERTIES TIMEOUT 180)

# Terminals: ConPTY spawn / echo / resize / kill + job cleanup / exit codes / UTF-8, OutputChannel flow control,
# then TerminalHost + TerminalController in a real WebView2 (hidden-pane replay, 128 MiB throughput benchmark,
# memory bound) and the terminal.ts / compat/node_pty.ts self-test when bun is available. The executable is
# also its own child process (--spew, --size, ...). Opens one small non-activated window.
add_executable(boson_terminal_tests
    tests/test_terminal.cpp
)
target_link_libraries(boson_terminal_tests PRIVATE boson_core psapi)
if(BOSON_BUN_EXECUTABLE)
    set(BOSON_TERMINAL_SELFTEST_JS "${CMAKE_BINARY_DIR}/terminal_selftest.js")
    add_custom_command(
        OUTPUT "${BOSON_TERMINAL_SELFTEST_JS}"
        COMMAND "${BOSON_BUN_EXECUTABLE}" build "${CMAKE_CURRENT_SOURCE_DIR}/tests/js/terminal_selftest.ts"
                --format=iife "--outfile=${BOSON_TERMINAL_SELFTEST_JS}"
        DEPENDS
            tests/js/terminal_selftest.ts
            frontend/src/boson.ts
            frontend/src/transfer.ts
            frontend/src/terminal.ts
            frontend/src/compat/node_pty.ts
        COMMENT "Bundling the terminal.ts self-test"
        VERBATIM)
    add_custom_target(boson_terminal_selftest_js DEPENDS "${BOSON_TERMINAL_SELFTEST_JS}")
    add_dependencies(boson_terminal_tests boson_terminal_selftest_js)
    target_compile_definitions(boson_terminal_tests PRIVATE BOSON_TERMINAL_SELFTEST_JS="${BOSON_TERMINAL_SELFTEST_JS}")
endif()
add_test(NAME boson_terminal_tests COMMAND boson_terminal_tests)
set_tests_properties(boson_terminal_tests PROPERTIES LABELS integration TIMEOUT 600)
# Opt-in: bundle the Microsoft.Windows.Console.ConPTY redistributable (conpty.dll + OpenConsole.exe, MIT, signed) next
# to the terminal executables for ~20x ConPTY output throughput. See cmake/BosonConPTY.cmake and docs/TERMINAL.md.
option(BOSON_BUNDLE_CONPTY "Download the ConPTY NuGet package and copy conpty.dll + OpenConsole.exe next to the terminal executables" OFF)
if(BOSON_BUNDLE_CONPTY)
    include(cmake/BosonConPTY.cmake)
    boson_bundle_conpty(boson_terminal_tests)
endif()
# Terminal demo (examples/terminal_demo): split panes of ConPTY terminals rendered by xterm.js + WebGL. Needs bun
# and frontend/node_modules (cd frontend && bun install: @xterm/* are devDependencies); skipped otherwise.
set(BOSON_TERMINAL_DEMO_WEB "${CMAKE_CURRENT_SOURCE_DIR}/examples/terminal_demo/web")
set(BOSON_XTERM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/frontend/node_modules/@xterm")
if(BOSON_BUN_EXECUTABLE AND EXISTS "${BOSON_XTERM_DIR}/xterm/css/xterm.css" AND EXISTS "${BOSON_XTERM_DIR}/addon-webgl")
    set(_demo_gen "${CMAKE_BINARY_DIR}/generated/terminal_demo")
    set(_demo_js "${_demo_gen}/app.js")
    set(_demo_header "${_demo_gen}/terminal_demo_assets.hpp")
    add_custom_command(
        OUTPUT "${_demo_header}"
        COMMAND "${BOSON_BUN_EXECUTABLE}" build "${BOSON_TERMINAL_DEMO_WEB}/demo.ts" --format=iife --minify
                "--outfile=${_demo_js}"
        COMMAND "${CMAKE_COMMAND}" "-DOUT=${_demo_header}"
                "-DASSETS=/index.html=${BOSON_TERMINAL_DEMO_WEB}/index.html,/demo.css=${BOSON_TERMINAL_DEMO_WEB}/demo.css,/xterm.css=${BOSON_XTERM_DIR}/xterm/css/xterm.css,/app.js=${_demo_js}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/examples/terminal_demo/embed_assets.cmake"
        DEPENDS
            examples/terminal_demo/web/demo.ts
            examples/terminal_demo/web/index.html
            examples/terminal_demo/web/demo.css
            examples/terminal_demo/web/tsconfig.json
            examples/terminal_demo/embed_assets.cmake
            frontend/src/boson.ts
            frontend/src/transfer.ts
            frontend/src/terminal.ts
        COMMENT "Bundling the terminal demo (xterm.js + WebGL)"
        VERBATIM)
    add_custom_target(boson_terminal_demo_assets DEPENDS "${_demo_header}")
    add_executable(boson_terminal_demo WIN32
        examples/terminal_demo/main.cpp
    )
    add_dependencies(boson_terminal_demo boson_terminal_demo_assets)
    target_include_directories(boson_terminal_demo PRIVATE "${_demo_gen}")
    target_link_libraries(boson_terminal_demo PRIVATE boson_core)
    if(BOSON_BUNDLE_CONPTY)
        boson_bundle_conpty(boson_terminal_demo)
    endif()
    add_test(NAME boson_terminal_demo_selftest COMMAND boson_terminal_demo --selftest)
    set_tests_properties(boson_terminal_demo_selftest PROPERTIES LABELS integration TIMEOUT 120)
endif()

# ---- Single executable (docs/SHIPPING.md): self-contained exes + a ctest that checks their imports -----------
boson_enable_pgo(boson_browser)
if(BOSON_SINGLE_EXE)
    if(BOSON_HELLO_EMBED_BOOTSTRAPPER)
        boson_single_exe(boson_hello EMBED_BOOTSTRAPPER)
    else()
        boson_single_exe(boson_hello)
    endif()
    boson_single_exe(boson_demo)
    boson_single_exe(boson_browser)
endif()
