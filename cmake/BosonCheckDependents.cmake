# ctest helper: fails when EXE imports a DLL outside the allow-list of Windows system DLLs.
# Usage: cmake -DEXE=<exe> -DDUMPBIN=<dumpbin.exe> -P BosonCheckDependents.cmake
if(NOT EXISTS "${EXE}")
    message(FATAL_ERROR "executable not found: ${EXE}")
endif()
if(NOT DUMPBIN)
    find_program(DUMPBIN dumpbin REQUIRED)
endif()
execute_process(COMMAND "${DUMPBIN}" /nologo /dependents "${EXE}"
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "dumpbin /dependents failed (${_rc}): ${_err}")
endif()

# Present in every Windows 10 / 11 install (System32). Anything else (VCRUNTIME140*, MSVCP140*, concrt140,
# a bundled conpty.dll, a WebView2Loader.dll ...) means the exe is not self-contained.
set(_allow
    kernel32 kernelbase ntdll user32 gdi32 advapi32 shell32 ole32 oleaut32 shlwapi dwmapi dcomp
    uiautomationcore dbghelp version shcore windowscodecs crypt32 bcrypt winhttp wintrust ws2_32 propsys
    wtsapi32 runtimeobject d3d11 d3d12 dxgi dwrite d3dcompiler_47 comctl32 comdlg32 imm32 setupapi
    rpcrt4 secur32 netapi32 iphlpapi userenv mswsock dnsapi cfgmgr32 powrprof psapi winmm uxtheme
    msimg32 gdiplus ncrypt wldap32 ucrtbase)
string(REGEX MATCHALL "[A-Za-z0-9_.-]+\\.[dD][lL][lL]" _deps "${_out}")
set(_bad "")
foreach(_dll IN LISTS _deps)
    string(TOLOWER "${_dll}" _l)
    string(REGEX REPLACE "\\.dll$" "" _stem "${_l}")
    if(_stem MATCHES "^api-ms-win-" OR _stem MATCHES "^ext-ms-win-")
        continue()
    endif()
    if(_stem IN_LIST _allow)
        continue()
    endif()
    list(APPEND _bad "${_dll}")
endforeach()
list(LENGTH _deps _n)
if(_bad)
    list(REMOVE_DUPLICATES _bad)
    message(FATAL_ERROR "${EXE} imports non-system DLLs: ${_bad}\nAll imports (${_n}): ${_deps}")
endif()
message(STATUS "${EXE}: ${_n} imported DLLs, all Windows system DLLs")
