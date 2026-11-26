# Opt-in packaging of the bundled ConPTY (Microsoft.Windows.Console.ConPTY NuGet package, MIT, signed by
# Microsoft Corporation). The in-box kernel32 ConPTY renders at ~60 Hz and caps terminal output at ~15-20 MiB/s;
# conpty.dll + OpenConsole.exe beside the executable lift that to ~300 MiB/s (docs/PERFORMANCE.md, docs/TERMINAL.md).
# boson::terminal::Pty (PtyBackend::Auto) uses them automatically when they sit next to the executable.
#
#   include(cmake/BosonConPTY.cmake)
#   boson_fetch_conpty()                      # downloads and verifies once, at configure time
#   boson_bundle_conpty(my_app)               # copies conpty.dll + OpenConsole.exe next to my_app after each build
#   install(FILES ${BOSON_CONPTY_FILES} DESTINATION .)   # and into the installer / archive
#
# Options (cache variables):
#   BOSON_CONPTY_VERSION  package version (default below)
#   BOSON_CONPTY_SHA256   SHA-256 of the .nupkg; the download is rejected when it does not match. Set it to the
#                         empty string to pin a different BOSON_CONPTY_VERSION and print the hash instead.
#   BOSON_CONPTY_ARCH     x64 (default), arm64 or x86
include_guard(GLOBAL)

set(BOSON_CONPTY_VERSION "1.25.261109001" CACHE STRING "Microsoft.Windows.Console.ConPTY NuGet package version")
set(BOSON_CONPTY_SHA256 "3d77f535df848e44225c2a8912ac844fecb23f3057732b639f3034b587496f09" CACHE STRING
    "Expected SHA-256 of the ConPTY .nupkg (empty: do not verify, print the hash)")
set(BOSON_CONPTY_ARCH "x64" CACHE STRING "Architecture of the bundled ConPTY (x64, arm64, x86)")

function(boson_fetch_conpty)
    if(BOSON_CONPTY_FILES)
        return()
    endif()
    set(_root "${CMAKE_BINARY_DIR}/_deps/conpty")
    set(_nupkg "${_root}/microsoft.windows.console.conpty.${BOSON_CONPTY_VERSION}.nupkg")
    set(_dir "${_root}/pkg-${BOSON_CONPTY_VERSION}")
    set(_dll "${_dir}/runtimes/win-${BOSON_CONPTY_ARCH}/native/conpty.dll")
    set(_console "${_dir}/build/native/runtimes/${BOSON_CONPTY_ARCH}/OpenConsole.exe")
    if(NOT EXISTS "${_dll}" OR NOT EXISTS "${_console}")
        file(MAKE_DIRECTORY "${_root}")
        if(NOT EXISTS "${_nupkg}")
            message(STATUS "Downloading Microsoft.Windows.Console.ConPTY ${BOSON_CONPTY_VERSION} from NuGet...")
            set(_url "https://api.nuget.org/v3-flatcontainer/microsoft.windows.console.conpty/${BOSON_CONPTY_VERSION}/microsoft.windows.console.conpty.${BOSON_CONPTY_VERSION}.nupkg")
            file(DOWNLOAD "${_url}" "${_nupkg}" STATUS _status TLS_VERIFY ON)
            list(GET _status 0 _code)
            list(GET _status 1 _message)
            if(NOT _code EQUAL 0)
                file(REMOVE "${_nupkg}")
                message(FATAL_ERROR "Failed to download the ConPTY NuGet package: ${_message} (${_code})")
            endif()
        endif()
        file(SHA256 "${_nupkg}" _actual)
        if(BOSON_CONPTY_SHA256 STREQUAL "")
            message(WARNING "BOSON_CONPTY_SHA256 is empty: the ConPTY package was not verified. SHA-256 of ${_nupkg}: ${_actual}")
        elseif(NOT _actual STREQUAL BOSON_CONPTY_SHA256)
            file(REMOVE "${_nupkg}")
            message(FATAL_ERROR "ConPTY package hash mismatch: expected ${BOSON_CONPTY_SHA256}, got ${_actual}")
        endif()
        file(MAKE_DIRECTORY "${_dir}")
        file(ARCHIVE_EXTRACT INPUT "${_nupkg}" DESTINATION "${_dir}")
    endif()
    if(NOT EXISTS "${_dll}" OR NOT EXISTS "${_console}")
        message(FATAL_ERROR "conpty.dll / OpenConsole.exe for ${BOSON_CONPTY_ARCH} not found under ${_dir}")
    endif()
    set(BOSON_CONPTY_DLL "${_dll}" CACHE INTERNAL "")
    set(BOSON_CONPTY_OPENCONSOLE "${_console}" CACHE INTERNAL "")
    set(BOSON_CONPTY_FILES "${_dll};${_console}" CACHE INTERNAL "conpty.dll and OpenConsole.exe, for install(FILES)")
endfunction()

# Copies conpty.dll and OpenConsole.exe next to <target>'s executable after every build.
function(boson_bundle_conpty target)
    boson_fetch_conpty()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BOSON_CONPTY_DLL}" "${BOSON_CONPTY_OPENCONSOLE}" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Bundling conpty.dll + OpenConsole.exe next to ${target}"
        VERBATIM)
endfunction()
