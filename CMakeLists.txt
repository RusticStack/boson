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
)

add_executable(boson_demo WIN32
    examples/demo/main.cpp
)
target_link_libraries(boson_demo PRIVATE boson_core)

add_executable(boson_browser WIN32
    examples/browser/main.cpp
)
target_link_libraries(boson_browser PRIVATE boson_core)

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
