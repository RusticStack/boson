# Boson shipping: install rules, CPack (portable ZIP + MSI through the WIX generator), MSIX layout + makeappx,
# and Authenticode signing with signtool.
#
#   include(cmake/BosonPackaging.cmake)
#   boson_package(my_app
#       NAME "My App" VERSION 1.2.3 VENDOR "Acme"
#       [DESCRIPTION "..."] [APP_USER_MODEL_ID Acme.MyApp] [UPGRADE_GUID <guid>] [REGISTRY_KEY "Acme\\My App"]
#       [PROTOCOLS myapp ...] [ICON app.ico] [LICENSE LICENSE.txt|license.rtf] [HOMEPAGE https://...]
#       [EXTRA_FILES ...] [EXTRA_DIRECTORIES ...] [SIDECAR_DIRECTORIES ...] [CLEAN_APPDATA_DIR "My App"]
#       [MSIX_IDENTITY Acme.MyApp] [MSIX_PUBLISHER "CN=Acme"] [MSIX_ASSETS dir] [MSIX_MIN_VERSION 10.0.17763.0]
#       [WIX_VERSION 3|4] [NO_ZIP] [NO_MSI] [NO_MSIX] [NO_VC_RUNTIME])
#   boson_sign_target(my_app)          # post-build signtool when BOSON_SIGN=ON
#   boson_add_sidecar(my_app DIRECTORY third_party/node [NAME node])
#                                      # post-build copy to <exe dir>/sidecars/<NAME>; boson_package installs
#                                      # every registered sidecar (and SIDECAR_DIRECTORIES) to sidecars/<NAME>
#
# Packages:   cpack -G ZIP   |   cpack -G WIX   |   cmake --build <dir> --target my_app_msix
include_guard(GLOBAL)

set(_BOSON_PACKAGING_DIR "${CMAKE_CURRENT_LIST_DIR}")

# ---- Tool discovery -------------------------------------------------------------------------------------
set(_boson_sdk_hints "")
if(DEFINED ENV{WindowsSdkVerBinPath})
    list(APPEND _boson_sdk_hints "$ENV{WindowsSdkVerBinPath}/x64")
endif()
set(_boson_pf86 "$ENV{ProgramFiles\(x86\)}")
if(_boson_pf86)
    file(GLOB _boson_sdk_bins LIST_DIRECTORIES TRUE "${_boson_pf86}/Windows Kits/10/bin/10.*")
    list(SORT _boson_sdk_bins COMPARE NATURAL ORDER DESCENDING)
    foreach(_bin IN LISTS _boson_sdk_bins)
        list(APPEND _boson_sdk_hints "${_bin}/x64")
    endforeach()
endif()
find_program(BOSON_SIGNTOOL signtool HINTS ${_boson_sdk_hints} DOC "Windows SDK signtool.exe")
find_program(BOSON_MAKEAPPX makeappx HINTS ${_boson_sdk_hints} DOC "Windows SDK makeappx.exe")
find_program(BOSON_WIX4 wix DOC "WiX Toolset v4+ wix.exe")
find_program(BOSON_WIX3_CANDLE candle HINTS "$ENV{WIX}/bin" DOC "WiX Toolset v3 candle.exe")

# ---- Signing options ------------------------------------------------------------------------------------
option(BOSON_SIGN "Authenticode-sign executables and packages with signtool" OFF)
set(BOSON_SIGN_THUMBPRINT "" CACHE STRING "SHA-1 thumbprint of a certificate in the Windows certificate store")
set(BOSON_SIGN_CERT_STORE "My" CACHE STRING "Certificate store name used with BOSON_SIGN_THUMBPRINT")
option(BOSON_SIGN_MACHINE_STORE "Look the thumbprint up in the machine store instead of the current user's" OFF)
set(BOSON_SIGN_PFX "" CACHE FILEPATH "PFX file used for signing")
set(BOSON_SIGN_PFX_PASSWORD_ENV "BOSON_SIGN_PFX_PASSWORD" CACHE STRING "Environment variable holding the PFX password (read at signing time)")
set(BOSON_SIGN_DLIB "" CACHE FILEPATH "Azure Trusted Signing: path to Azure.CodeSigning.Dlib.dll")
set(BOSON_SIGN_DLIB_METADATA "" CACHE FILEPATH "Azure Trusted Signing: metadata.json (Endpoint, CodeSigningAccountName, CertificateProfileName)")
set(BOSON_SIGN_TIMESTAMP_URL "http://timestamp.digicert.com" CACHE STRING "RFC 3161 timestamp server (Azure Trusted Signing: http://timestamp.acs.microsoft.com); empty = none")
set(BOSON_SIGN_DESCRIPTION "" CACHE STRING "Signature description (signtool /d)")

function(_boson_sign_defines out_var)
    set(_d
        "-DBOSON_SIGNTOOL=${BOSON_SIGNTOOL}"
        "-DBOSON_SIGN_THUMBPRINT=${BOSON_SIGN_THUMBPRINT}"
        "-DBOSON_SIGN_CERT_STORE=${BOSON_SIGN_CERT_STORE}"
        "-DBOSON_SIGN_MACHINE_STORE=${BOSON_SIGN_MACHINE_STORE}"
        "-DBOSON_SIGN_PFX=${BOSON_SIGN_PFX}"
        "-DBOSON_SIGN_PFX_PASSWORD_ENV=${BOSON_SIGN_PFX_PASSWORD_ENV}"
        "-DBOSON_SIGN_DLIB=${BOSON_SIGN_DLIB}"
        "-DBOSON_SIGN_DLIB_METADATA=${BOSON_SIGN_DLIB_METADATA}"
        "-DBOSON_SIGN_TIMESTAMP_URL=${BOSON_SIGN_TIMESTAMP_URL}"
        "-DBOSON_SIGN_DESCRIPTION=${BOSON_SIGN_DESCRIPTION}")
    set(${out_var} "${_d}" PARENT_SCOPE)
endfunction()

# boson_sign_command(<out_var> FILES <file>...): a COMMAND list (for add_custom_command) that signs FILES.
function(boson_sign_command out_var)
    cmake_parse_arguments(PARSE_ARGV 1 BSC "" "" "FILES")
    _boson_sign_defines(_defs)
    string(REPLACE ";" "\\;" _files "${BSC_FILES}")
    set(${out_var} "${CMAKE_COMMAND}" ${_defs} "-DBOSON_SIGN_FILES=${_files}" -P "${_BOSON_PACKAGING_DIR}/BosonSign.cmake" PARENT_SCOPE)
endfunction()

# boson_sign_target(<target>...): signs each target's output after every build when BOSON_SIGN is ON.
function(boson_sign_target)
    if(NOT BOSON_SIGN)
        return()
    endif()
    foreach(_t IN LISTS ARGN)
        boson_sign_command(_cmd FILES "$<TARGET_FILE:${_t}>")
        add_custom_command(TARGET ${_t} POST_BUILD COMMAND ${_cmd} COMMENT "Signing ${_t}" VERBATIM)
    endforeach()
endfunction()

function(_boson_xml_escape out_var text)
    string(REPLACE "&" "&amp;" text "${text}")
    string(REPLACE "<" "&lt;" text "${text}")
    string(REPLACE ">" "&gt;" text "${text}")
    string(REPLACE "\"" "&quot;" text "${text}")
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

function(_boson_guid out_var seed name)
    string(UUID _g NAMESPACE "6ba7b810-9dad-11d1-80b4-00c04fd430c8" NAME "${seed}:${name}" TYPE SHA1 UPPER)
    set(${out_var} "${_g}" PARENT_SCOPE)
endfunction()

# ---- boson_package --------------------------------------------------------------------------------------
# boson_add_sidecar(<target> DIRECTORY <dir> [NAME <name>]): ships <dir> (e.g. a node.exe + scripts bundle) as
# sidecars/<name> next to the executable. Copied after every build so the build tree runs like the package, and
# installed (ZIP / MSI / MSIX) by boson_package. At runtime: app::executable_directory() / "sidecars" / name.
function(boson_add_sidecar target)
    cmake_parse_arguments(PARSE_ARGV 1 BAS "" "DIRECTORY;NAME" "")
    if(NOT TARGET ${target})
        message(FATAL_ERROR "boson_add_sidecar: '${target}' is not a target")
    endif()
    if(NOT BAS_DIRECTORY)
        message(FATAL_ERROR "boson_add_sidecar: DIRECTORY is required")
    endif()
    get_filename_component(_dir "${BAS_DIRECTORY}" ABSOLUTE)
    if(NOT BAS_NAME)
        get_filename_component(BAS_NAME "${_dir}" NAME)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_dir}" "$<TARGET_FILE_DIR:${target}>/sidecars/${BAS_NAME}"
        COMMENT "Copying sidecar ${BAS_NAME}" VERBATIM)
    set_property(TARGET ${target} APPEND PROPERTY BOSON_SIDECARS "${_dir}|${BAS_NAME}")
endfunction()

function(boson_package target)
    cmake_parse_arguments(PARSE_ARGV 1 BP
        "NO_ZIP;NO_MSI;NO_MSIX;NO_VC_RUNTIME"
        "NAME;VERSION;VENDOR;DESCRIPTION;APP_USER_MODEL_ID;REGISTRY_KEY;UPGRADE_GUID;ICON;LICENSE;HOMEPAGE;CLEAN_APPDATA_DIR;MSIX_IDENTITY;MSIX_PUBLISHER;MSIX_ASSETS;MSIX_MIN_VERSION;MSIX_MAX_VERSION_TESTED;MSIX_ARCH;MSIX_LANGUAGE;WIX_VERSION"
        "PROTOCOLS;EXTRA_FILES;EXTRA_DIRECTORIES;SIDECAR_DIRECTORIES")
    if(NOT TARGET ${target})
        message(FATAL_ERROR "boson_package: '${target}' is not a target")
    endif()
    get_property(_already GLOBAL PROPERTY BOSON_PACKAGE_TARGET)
    if(_already)
        message(FATAL_ERROR "boson_package: called for '${target}' but '${_already}' is already packaged (CPack supports one package per project)")
    endif()
    set_property(GLOBAL PROPERTY BOSON_PACKAGE_TARGET "${target}")

    foreach(_req NAME VERSION VENDOR)
        if(NOT BP_${_req})
            message(FATAL_ERROR "boson_package: ${_req} is required")
        endif()
    endforeach()
    if(NOT BP_DESCRIPTION)
        set(BP_DESCRIPTION "${BP_NAME}")
    endif()
    string(REGEX REPLACE "[^A-Za-z0-9.]" "" _vendor_id "${BP_VENDOR}")
    string(REGEX REPLACE "[^A-Za-z0-9.]" "" _name_id "${BP_NAME}")
    if(NOT BP_APP_USER_MODEL_ID)
        set(BP_APP_USER_MODEL_ID "${_vendor_id}.${_name_id}")
    endif()
    if(NOT BP_REGISTRY_KEY)
        set(BP_REGISTRY_KEY "${BP_VENDOR}\\${BP_NAME}")
    endif()
    if(NOT BP_WIX_VERSION)
        if(BOSON_WIX4)
            set(BP_WIX_VERSION 4)
        else()
            set(BP_WIX_VERSION 3)
        endif()
    endif()

    # Versions: MSI and MSIX want numeric MAJOR.MINOR.PATCH[.REVISION].
    if(NOT BP_VERSION MATCHES "^v?([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?(\\.([0-9]+))?")
        message(FATAL_ERROR "boson_package: VERSION '${BP_VERSION}' must start with MAJOR[.MINOR[.PATCH[.REVISION]]]")
    endif()
    set(_v1 "${CMAKE_MATCH_1}")
    set(_v2 "${CMAKE_MATCH_3}")
    set(_v3 "${CMAKE_MATCH_5}")
    set(_v4 "${CMAKE_MATCH_7}")
    foreach(_v _v2 _v3 _v4)
        if("${${_v}}" STREQUAL "")
            set(${_v} 0)
        endif()
    endforeach()
    set(_version3 "${_v1}.${_v2}.${_v3}")
    set(_version4 "${_v1}.${_v2}.${_v3}.${_v4}")

    get_target_property(_out_name ${target} OUTPUT_NAME)
    if(NOT _out_name)
        set(_out_name "${target}")
    endif()
    set(_exe "${_out_name}${CMAKE_EXECUTABLE_SUFFIX}")
    string(REGEX REPLACE "[^A-Za-z0-9._-]" "-" _file_stem "${BP_NAME}")
    set(_package_file "${_file_stem}-${_version3}-win64")

    # ---- install() rules (also what the ZIP contains) -----------------------------------------------
    install(TARGETS ${target} RUNTIME DESTINATION . COMPONENT application)
    if(BP_EXTRA_FILES)
        install(FILES ${BP_EXTRA_FILES} DESTINATION . COMPONENT application)
    endif()
    foreach(_dir IN LISTS BP_EXTRA_DIRECTORIES)
        install(DIRECTORY "${_dir}" DESTINATION . COMPONENT application)
    endforeach()
    # Sidecars (boson_add_sidecar + SIDECAR_DIRECTORIES): "<abs dir>|<name>" pairs, installed to sidecars/<name>.
    get_target_property(_sidecars ${target} BOSON_SIDECARS)
    if(NOT _sidecars)
        set(_sidecars "")
    endif()
    foreach(_d IN LISTS BP_SIDECAR_DIRECTORIES)
        get_filename_component(_abs "${_d}" ABSOLUTE)
        get_filename_component(_dn "${_abs}" NAME)
        list(APPEND _sidecars "${_abs}|${_dn}")
    endforeach()
    foreach(_entry IN LISTS _sidecars)
        string(REPLACE "|" ";" _pair "${_entry}")
        list(GET _pair 0 _sd)
        list(GET _pair 1 _sn)
        install(DIRECTORY "${_sd}/" DESTINATION "sidecars/${_sn}" COMPONENT application)
    endforeach()
    # App-local MSVC runtime (vcruntime140*.dll, msvcp140*.dll) so the package runs on machines without the
    # VC++ redistributable. The UCRT is part of Windows 10+.
    set(_vc_runtime "")
    if(NOT BP_NO_VC_RUNTIME AND MSVC)
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT application)
        set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
        include(InstallRequiredSystemLibraries)
        set(_vc_runtime ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
    endif()

    _boson_xml_escape(BOSON_PKG_NAME_XML "${BP_NAME}")
    _boson_xml_escape(BOSON_PKG_VENDOR_XML "${BP_VENDOR}")
    _boson_xml_escape(BOSON_PKG_DESCRIPTION_XML "${BP_DESCRIPTION}")
    set(BOSON_PKG_EXE "${_exe}")
    set(BOSON_PKG_AUMID "${BP_APP_USER_MODEL_ID}")
    _boson_xml_escape(BOSON_PKG_REG_KEY "${BP_REGISTRY_KEY}")

    set(_work "${CMAKE_BINARY_DIR}/boson_package/${target}")
    file(MAKE_DIRECTORY "${_work}")

    # ---- WiX sources: shortcut + AUMID, protocols, uninstall cleanup ----------------------------------
    if(NOT BP_UPGRADE_GUID)
        _boson_guid(BP_UPGRADE_GUID "boson-upgrade" "${BP_VENDOR}:${BP_NAME}")
        message(STATUS "boson_package: no UPGRADE_GUID given, derived ${BP_UPGRADE_GUID} from VENDOR+NAME (pin it in your CMakeLists so renames do not break upgrades)")
    endif()
    _boson_guid(BOSON_WIX_GUID_SHORTCUT "${BP_UPGRADE_GUID}" "shortcut")
    _boson_guid(BOSON_WIX_GUID_CLEANUP "${BP_UPGRADE_GUID}" "cleanup")
    if(BP_WIX_VERSION GREATER_EQUAL 4)
        set(BOSON_WIX_NS "http://wixtoolset.org/schemas/v4/wxs")
        set(_util_ns "http://wixtoolset.org/schemas/v4/wxs/util")
        set(_util_ext "WixToolset.Util.wixext")
        set(BOSON_WIX_PROGRAM_MENU_DECL "    <StandardDirectory Id=\"ProgramMenuFolder\" />")
    else()
        set(BOSON_WIX_NS "http://schemas.microsoft.com/wix/2006/wi")
        set(_util_ns "http://schemas.microsoft.com/wix/UtilExtension")
        set(_util_ext "WixUtilExtension")
        set(BOSON_WIX_PROGRAM_MENU_DECL "    <DirectoryRef Id=\"TARGETDIR\">\n      <Directory Id=\"ProgramMenuFolder\" />\n    </DirectoryRef>")
    endif()
    set(BOSON_WIX_UTIL_NS_ATTR "")
    set(BOSON_WIX_APPDATA_PROPERTY "")
    set(BOSON_WIX_APPDATA_COMPONENT "")
    if(BP_CLEAN_APPDATA_DIR)
        _boson_xml_escape(_appdata "${BP_CLEAN_APPDATA_DIR}")
        set(BOSON_WIX_UTIL_NS_ATTR " xmlns:util=\"${_util_ns}\"")
        # RemoveFolderEx needs the path in a property before costing: remember it in the registry at install
        # time and read it back with RegistrySearch at uninstall time (the documented WiX pattern).
        set(BOSON_WIX_APPDATA_PROPERTY "    <Property Id=\"BOSON_APPDATA_DIR\">\n      <RegistrySearch Id=\"BosonAppDataSearch\" Root=\"HKCU\" Key=\"Software\\${BOSON_PKG_REG_KEY}\" Name=\"AppDataDir\" Type=\"raw\" />\n    </Property>")
        set(BOSON_WIX_APPDATA_COMPONENT "        <RegistryValue Root=\"HKCU\" Key=\"Software\\${BOSON_PKG_REG_KEY}\" Name=\"AppDataDir\" Type=\"string\" Value=\"[LocalAppDataFolder]${_appdata}\" />\n        <util:RemoveFolderEx On=\"uninstall\" Property=\"BOSON_APPDATA_DIR\" />\n")
    endif()
    set(BOSON_WIX_PROTOCOL_COMPONENTS "")
    foreach(_scheme IN LISTS BP_PROTOCOLS)
        if(NOT _scheme MATCHES "^[A-Za-z][A-Za-z0-9+.-]*$")
            message(FATAL_ERROR "boson_package: '${_scheme}' is not a valid URL scheme")
        endif()
        string(TOLOWER "${_scheme}" _scheme)
        string(MAKE_C_IDENTIFIER "${_scheme}" _sid)
        _boson_guid(_pg "${BP_UPGRADE_GUID}" "protocol:${_scheme}")
        string(APPEND BOSON_WIX_PROTOCOL_COMPONENTS
            "      <Component Id=\"BosonProtocol_${_sid}\" Directory=\"INSTALL_ROOT\" Guid=\"${_pg}\">\n"
            "        <RegistryKey Root=\"HKCU\" Key=\"Software\\Classes\\${_scheme}\" ForceCreateOnInstall=\"yes\" ForceDeleteOnUninstall=\"yes\">\n"
            "          <RegistryValue Type=\"string\" Value=\"URL:${BOSON_PKG_NAME_XML}\" KeyPath=\"yes\" />\n"
            "          <RegistryValue Name=\"URL Protocol\" Type=\"string\" Value=\"\" />\n"
            "          <RegistryValue Key=\"DefaultIcon\" Type=\"string\" Value=\"&quot;[INSTALL_ROOT]${_exe}&quot;,0\" />\n"
            "          <RegistryValue Key=\"shell\\open\\command\" Type=\"string\" Value=\"&quot;[INSTALL_ROOT]${_exe}&quot; &quot;%1&quot;\" />\n"
            "        </RegistryKey>\n"
            "      </Component>\n")
    endforeach()
    configure_file("${_BOSON_PACKAGING_DIR}/templates/boson_wix_integration.wxs.in" "${_work}/boson_wix_integration.wxs" @ONLY)
    configure_file("${_BOSON_PACKAGING_DIR}/templates/boson_wix_patch.xml.in" "${_work}/boson_wix_patch.xml" @ONLY)

    # ---- CPack ---------------------------------------------------------------------------------------
    set(CPACK_PACKAGE_NAME "${BP_NAME}")
    set(CPACK_PACKAGE_VENDOR "${BP_VENDOR}")
    set(CPACK_PACKAGE_VERSION "${_version4}")
    set(CPACK_PACKAGE_VERSION_MAJOR "${_v1}")
    set(CPACK_PACKAGE_VERSION_MINOR "${_v2}")
    set(CPACK_PACKAGE_VERSION_PATCH "${_v3}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${BP_DESCRIPTION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "${BP_NAME}")
    set(CPACK_PACKAGE_FILE_NAME "${_package_file}")
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
    if(BP_LICENSE)
        set(CPACK_RESOURCE_FILE_LICENSE "${BP_LICENSE}")
    else()
        file(WRITE "${_work}/LICENSE.txt" "Copyright (c) ${BP_VENDOR}. All rights reserved.\n")
        set(CPACK_RESOURCE_FILE_LICENSE "${_work}/LICENSE.txt")
    endif()

    set(_generators "")
    if(NOT BP_NO_ZIP)
        list(APPEND _generators ZIP)
    endif()
    if(NOT BP_NO_MSI)
        if(BOSON_WIX4 OR BOSON_WIX3_CANDLE)
            list(APPEND _generators WIX)
        else()
            message(STATUS "boson_package: WiX Toolset not found; 'cpack -G WIX' needs WiX v3 (candle/light) or v4+ (wix.exe)")
        endif()
        if(BP_WIX_VERSION GREATER_EQUAL 4)
            set(CPACK_WIX_VERSION 4)
        endif()
        set(CPACK_WIX_UPGRADE_GUID "${BP_UPGRADE_GUID}")
        set(CPACK_WIX_INSTALL_SCOPE "perUser")
        set(CPACK_WIX_EXTRA_SOURCES "${_work}/boson_wix_integration.wxs")
        set(CPACK_WIX_PATCH_FILE "${_work}/boson_wix_patch.xml")
        set(CPACK_WIX_PROPERTY_ARPNOMODIFY "1")
        if(BP_HOMEPAGE)
            set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "${BP_HOMEPAGE}")
        endif()
        if(BP_ICON)
            set(CPACK_WIX_PRODUCT_ICON "${BP_ICON}")
        endif()
        if(BP_CLEAN_APPDATA_DIR)
            set(CPACK_WIX_EXTENSIONS "${_util_ext}")
        endif()
    endif()
    set(CPACK_GENERATOR "${_generators}")

    if(BOSON_SIGN)
        set(CPACK_BOSON_SIGN ON)
        set(CPACK_BOSON_SIGN_SCRIPT "${_BOSON_PACKAGING_DIR}/BosonSign.cmake")
        foreach(_var SIGNTOOL SIGN_THUMBPRINT SIGN_CERT_STORE SIGN_MACHINE_STORE SIGN_PFX SIGN_PFX_PASSWORD_ENV SIGN_DLIB SIGN_DLIB_METADATA SIGN_TIMESTAMP_URL SIGN_DESCRIPTION)
            set(CPACK_BOSON_${_var} "${BOSON_${_var}}")
        endforeach()
        set(CPACK_POST_BUILD_SCRIPTS "${_BOSON_PACKAGING_DIR}/BosonCPackPostBuild.cmake")
    endif()
    include(CPack)

    # ---- MSIX ----------------------------------------------------------------------------------------
    if(NOT BP_NO_MSIX)
        if(NOT BP_MSIX_IDENTITY)
            set(BP_MSIX_IDENTITY "${_vendor_id}.${_name_id}")
        endif()
        if(NOT BP_MSIX_IDENTITY MATCHES "^[A-Za-z0-9.-]+$")
            message(FATAL_ERROR "boson_package: MSIX_IDENTITY may only contain letters, digits, '.' and '-'")
        endif()
        if(NOT BP_MSIX_PUBLISHER)
            set(BP_MSIX_PUBLISHER "CN=${BP_VENDOR}")
        endif()
        if(NOT BP_MSIX_MIN_VERSION)
            set(BP_MSIX_MIN_VERSION "10.0.17763.0")
        endif()
        if(NOT BP_MSIX_MAX_VERSION_TESTED)
            set(BP_MSIX_MAX_VERSION_TESTED "10.0.26100.0")
        endif()
        if(NOT BP_MSIX_ARCH)
            set(BP_MSIX_ARCH "x64")
        endif()
        if(NOT BP_MSIX_LANGUAGE)
            set(BP_MSIX_LANGUAGE "en-us")
        endif()
        set(BOSON_MSIX_IDENTITY "${BP_MSIX_IDENTITY}")
        _boson_xml_escape(BOSON_MSIX_PUBLISHER "${BP_MSIX_PUBLISHER}")
        # MSIX requires REVISION 0 for Store submissions; side-loaded packages may use it.
        set(BOSON_MSIX_VERSION "${_version4}")
        set(BOSON_MSIX_ARCH "${BP_MSIX_ARCH}")
        set(BOSON_MSIX_MIN_VERSION "${BP_MSIX_MIN_VERSION}")
        set(BOSON_MSIX_MAX_VERSION_TESTED "${BP_MSIX_MAX_VERSION_TESTED}")
        set(BOSON_MSIX_LANGUAGE "${BP_MSIX_LANGUAGE}")
        string(REGEX REPLACE "[^A-Za-z0-9]" "" BOSON_MSIX_APP_ID "${target}")
        if(NOT BOSON_MSIX_APP_ID MATCHES "^[A-Za-z]")
            set(BOSON_MSIX_APP_ID "App${BOSON_MSIX_APP_ID}")
        endif()
        set(BOSON_MSIX_EXTENSIONS "")
        if(BP_PROTOCOLS)
            string(APPEND BOSON_MSIX_EXTENSIONS "      <Extensions>\n")
            foreach(_scheme IN LISTS BP_PROTOCOLS)
                string(TOLOWER "${_scheme}" _scheme)
                string(APPEND BOSON_MSIX_EXTENSIONS
                    "        <uap:Extension Category=\"windows.protocol\">\n"
                    "          <uap:Protocol Name=\"${_scheme}\">\n"
                    "            <uap:DisplayName>${BOSON_PKG_NAME_XML}</uap:DisplayName>\n"
                    "          </uap:Protocol>\n"
                    "        </uap:Extension>\n")
            endforeach()
            string(APPEND BOSON_MSIX_EXTENSIONS "      </Extensions>\n")
        endif()
        set(_msix_dir "${_work}/msix")
        set(_layout "${_msix_dir}/layout")
        set(_manifest "${_msix_dir}/AppxManifest.xml")
        set(_msix_out "${CMAKE_BINARY_DIR}/packages/${_package_file}.msix")
        configure_file("${_BOSON_PACKAGING_DIR}/templates/AppxManifest.xml.in" "${_manifest}" @ONLY)

        set(_cmds
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_layout}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_layout}/Assets" "${CMAKE_BINARY_DIR}/packages"
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${target}>" "${_layout}/${_exe}"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_manifest}" "${_layout}/AppxManifest.xml")
        foreach(_f IN LISTS BP_EXTRA_FILES _vc_runtime)
            list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy "${_f}" "${_layout}/")
        endforeach()
        foreach(_d IN LISTS BP_EXTRA_DIRECTORIES)
            get_filename_component(_dn "${_d}" NAME)
            list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_d}" "${_layout}/${_dn}")
        endforeach()
        foreach(_entry IN LISTS _sidecars)
            string(REPLACE "|" ";" _pair "${_entry}")
            list(GET _pair 0 _sd)
            list(GET _pair 1 _sn)
            list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_sd}" "${_layout}/sidecars/${_sn}")
        endforeach()
        if(BP_MSIX_ASSETS)
            list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy_directory "${BP_MSIX_ASSETS}" "${_layout}/Assets")
        else()
            list(APPEND _cmds COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File "${_BOSON_PACKAGING_DIR}/msix/generate_assets.ps1" -OutDir "${_layout}/Assets" -Name "${BP_NAME}")
        endif()
        if(BOSON_MAKEAPPX)
            list(APPEND _cmds COMMAND "${BOSON_MAKEAPPX}" pack /o /h SHA256 /d "${_layout}" /p "${_msix_out}")
        else()
            list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E echo "makeappx.exe not found: install the Windows 10/11 SDK"
                COMMAND "${CMAKE_COMMAND}" -E false)
        endif()
        if(BOSON_SIGN)
            boson_sign_command(_sign FILES "${_msix_out}")
            list(APPEND _cmds COMMAND ${_sign})
        endif()
        add_custom_target(${target}_msix
            ${_cmds}
            DEPENDS ${target}
            BYPRODUCTS "${_msix_out}"
            COMMENT "Packing ${_msix_out} (Publisher must equal the signing certificate's subject: ${BP_MSIX_PUBLISHER})"
            VERBATIM)
    endif()

    set(BOSON_PACKAGE_FILE_STEM "${_package_file}" PARENT_SCOPE)
endfunction()
