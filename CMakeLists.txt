cmake_minimum_required(VERSION 3.28)

project(Boson LANGUAGES CXX)

# CMake 4.4 does not yet map CXX26 for MSVC; /std:c++latest is passed explicitly below.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    src/desktop/hidden_window.cpp
    src/desktop/clipboard.cpp
    src/desktop/shell.cpp
    src/desktop/shortcuts.cpp
    src/desktop/deep_link.cpp
    src/desktop/screen.cpp
    src/desktop/window_state.cpp
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
)

add_executable(boson_demo WIN32
    examples/demo/main.cpp
)
target_link_libraries(boson_demo PRIVATE boson_core)

add_executable(boson_browser WIN32
    examples/browser/main.cpp
)
target_link_libraries(boson_browser PRIVATE boson_core)

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
            frontend/src/boson.ts
            frontend/src/compat/electron.ts
            frontend/src/compat/tauri.ts
            frontend/src/compat/wails.ts
        COMMENT "Bundling the compat shim self-test"
        VERBATIM)
    add_custom_target(boson_porting_shim_js DEPENDS "${BOSON_PORTING_SHIM_JS}")
    add_dependencies(boson_porting_tests boson_porting_shim_js)
    target_compile_definitions(boson_porting_tests PRIVATE BOSON_PORTING_SHIM_JS="${BOSON_PORTING_SHIM_JS}")
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
