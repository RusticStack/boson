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
    src/shipping/logger.cpp
    src/shipping/version.cpp
    src/shipping/crypto.cpp
    src/shipping/http.cpp
    src/shipping/authenticode.cpp
    src/shipping/updater.cpp
    src/shipping/webview2_runtime.cpp
    src/shipping/crash_reporter.cpp
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
        bcrypt
        crypt32
        winhttp
        wintrust
        dbghelp
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
