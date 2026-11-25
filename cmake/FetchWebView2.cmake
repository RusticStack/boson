include_guard(GLOBAL)

set(BOSON_WEBVIEW2_VERSION "1.0.4370.26" CACHE STRING "Microsoft.Web.WebView2 NuGet package version")
set(BOSON_WEBVIEW2_URL "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${BOSON_WEBVIEW2_VERSION}/microsoft.web.webview2.${BOSON_WEBVIEW2_VERSION}.nupkg")
set(BOSON_WEBVIEW2_ROOT "${CMAKE_BINARY_DIR}/_deps/webview2")
set(BOSON_WEBVIEW2_NUPKG "${BOSON_WEBVIEW2_ROOT}/microsoft.web.webview2.${BOSON_WEBVIEW2_VERSION}.nupkg")
set(BOSON_WEBVIEW2_DIR "${BOSON_WEBVIEW2_ROOT}/pkg-${BOSON_WEBVIEW2_VERSION}")

set(BOSON_WEBVIEW2_INCLUDE_DIR "${BOSON_WEBVIEW2_DIR}/build/native/include")
set(BOSON_WEBVIEW2_STATIC_LIB "${BOSON_WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib")

if(NOT EXISTS "${BOSON_WEBVIEW2_INCLUDE_DIR}/WebView2.h" OR NOT EXISTS "${BOSON_WEBVIEW2_STATIC_LIB}")
    file(MAKE_DIRECTORY "${BOSON_WEBVIEW2_ROOT}")

    if(NOT EXISTS "${BOSON_WEBVIEW2_NUPKG}")
        message(STATUS "Downloading Microsoft.Web.WebView2 ${BOSON_WEBVIEW2_VERSION} from NuGet...")
        file(DOWNLOAD
            "${BOSON_WEBVIEW2_URL}"
            "${BOSON_WEBVIEW2_NUPKG}"
            STATUS download_status
            TLS_VERIFY ON
        )
        list(GET download_status 0 status_code)
        list(GET download_status 1 status_message)
        if(NOT status_code EQUAL 0)
            file(REMOVE "${BOSON_WEBVIEW2_NUPKG}")
            message(FATAL_ERROR "Failed to download WebView2 NuGet package: ${status_message} (${status_code})")
        endif()
    endif()

    message(STATUS "Extracting Microsoft.Web.WebView2 ${BOSON_WEBVIEW2_VERSION}...")
    file(MAKE_DIRECTORY "${BOSON_WEBVIEW2_DIR}")
    file(ARCHIVE_EXTRACT
        INPUT "${BOSON_WEBVIEW2_NUPKG}"
        DESTINATION "${BOSON_WEBVIEW2_DIR}"
    )
endif()

if(NOT EXISTS "${BOSON_WEBVIEW2_INCLUDE_DIR}/WebView2.h" OR NOT EXISTS "${BOSON_WEBVIEW2_STATIC_LIB}")
    message(FATAL_ERROR "WebView2 SDK headers or WebView2LoaderStatic.lib not found under ${BOSON_WEBVIEW2_DIR}")
endif()

if(NOT TARGET WebView2::Static)
    add_library(WebView2::Static STATIC IMPORTED GLOBAL)
    set_target_properties(WebView2::Static PROPERTIES
        IMPORTED_LOCATION "${BOSON_WEBVIEW2_STATIC_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${BOSON_WEBVIEW2_INCLUDE_DIR}"
    )
endif()
