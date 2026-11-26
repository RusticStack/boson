# Authenticode signing script, run as:
#   cmake -DBOSON_SIGNTOOL=<signtool.exe> -DBOSON_SIGN_FILES=<a;b> [options below] -P BosonSign.cmake
#
# Certificate selection (exactly one):
#   -DBOSON_SIGN_THUMBPRINT=<sha1>  [-DBOSON_SIGN_CERT_STORE=My] [-DBOSON_SIGN_MACHINE_STORE=ON]
#   -DBOSON_SIGN_PFX=<file.pfx>     [-DBOSON_SIGN_PFX_PASSWORD_ENV=<VAR>]   (the password is read from the
#                                    environment at signing time and never stored in the CMake cache)
#   -DBOSON_SIGN_DLIB=<Azure.CodeSigning.Dlib.dll> -DBOSON_SIGN_DLIB_METADATA=<metadata.json>
#                                    (Azure Trusted Signing; authenticate with az login / env credentials)
# Common:
#   -DBOSON_SIGN_TIMESTAMP_URL=<RFC 3161 URL> (empty = no timestamp)   -DBOSON_SIGN_DESCRIPTION=<text>
#   -DBOSON_SIGN_RETRIES=3
cmake_minimum_required(VERSION 3.28)

if(NOT BOSON_SIGNTOOL OR NOT EXISTS "${BOSON_SIGNTOOL}")
    message(FATAL_ERROR "BosonSign: signtool.exe not found (BOSON_SIGNTOOL='${BOSON_SIGNTOOL}'). Install the Windows SDK.")
endif()
if(NOT BOSON_SIGN_FILES)
    message(FATAL_ERROR "BosonSign: BOSON_SIGN_FILES is empty")
endif()
if(NOT DEFINED BOSON_SIGN_RETRIES OR BOSON_SIGN_RETRIES STREQUAL "")
    set(BOSON_SIGN_RETRIES 3)
endif()

set(_args sign /fd SHA256)
set(_selected 0)
if(BOSON_SIGN_THUMBPRINT)
    math(EXPR _selected "${_selected} + 1")
    string(REPLACE " " "" _tp "${BOSON_SIGN_THUMBPRINT}")
    if(NOT BOSON_SIGN_CERT_STORE)
        set(BOSON_SIGN_CERT_STORE My)
    endif()
    list(APPEND _args /sha1 "${_tp}" /s "${BOSON_SIGN_CERT_STORE}")
    if(BOSON_SIGN_MACHINE_STORE)
        list(APPEND _args /sm)
    endif()
endif()
if(BOSON_SIGN_PFX)
    math(EXPR _selected "${_selected} + 1")
    if(NOT EXISTS "${BOSON_SIGN_PFX}")
        message(FATAL_ERROR "BosonSign: PFX file '${BOSON_SIGN_PFX}' does not exist")
    endif()
    list(APPEND _args /f "${BOSON_SIGN_PFX}")
    if(BOSON_SIGN_PFX_PASSWORD_ENV)
        if(DEFINED ENV{${BOSON_SIGN_PFX_PASSWORD_ENV}})
            list(APPEND _args /p "$ENV{${BOSON_SIGN_PFX_PASSWORD_ENV}}")
        else()
            message(FATAL_ERROR "BosonSign: environment variable ${BOSON_SIGN_PFX_PASSWORD_ENV} (PFX password) is not set")
        endif()
    endif()
endif()
if(BOSON_SIGN_DLIB)
    math(EXPR _selected "${_selected} + 1")
    if(NOT BOSON_SIGN_DLIB_METADATA OR NOT EXISTS "${BOSON_SIGN_DLIB_METADATA}")
        message(FATAL_ERROR "BosonSign: BOSON_SIGN_DLIB needs BOSON_SIGN_DLIB_METADATA (Azure Trusted Signing metadata.json)")
    endif()
    list(APPEND _args /dlib "${BOSON_SIGN_DLIB}" /dmdf "${BOSON_SIGN_DLIB_METADATA}")
endif()
if(NOT _selected EQUAL 1)
    message(FATAL_ERROR "BosonSign: set exactly one of BOSON_SIGN_THUMBPRINT, BOSON_SIGN_PFX or BOSON_SIGN_DLIB")
endif()
if(BOSON_SIGN_TIMESTAMP_URL)
    list(APPEND _args /tr "${BOSON_SIGN_TIMESTAMP_URL}" /td SHA256)
endif()
if(BOSON_SIGN_DESCRIPTION)
    list(APPEND _args /d "${BOSON_SIGN_DESCRIPTION}")
endif()

foreach(_file IN LISTS BOSON_SIGN_FILES)
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "BosonSign: '${_file}' does not exist")
    endif()
    set(_attempt 1)
    while(TRUE)
        # Timestamp servers fail transiently; signing itself is deterministic, so retrying is safe.
        execute_process(
            COMMAND "${BOSON_SIGNTOOL}" ${_args} "${_file}"
            RESULT_VARIABLE _rc
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)
        if(_rc EQUAL 0)
            message(STATUS "BosonSign: signed ${_file}")
            break()
        endif()
        if(_attempt GREATER_EQUAL BOSON_SIGN_RETRIES)
            # Never echo the arguments: they can contain the PFX password.
            message(FATAL_ERROR "BosonSign: signtool failed for ${_file} (exit ${_rc}):\n${_out}${_err}")
        endif()
        math(EXPR _attempt "${_attempt} + 1")
        message(STATUS "BosonSign: retrying ${_file} (attempt ${_attempt})")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 3)
    endwhile()
endforeach()
