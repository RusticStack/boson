#include <boson/shipping/authenticode.hpp>

#include <boson/core/result.hpp>

#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <cwctype>

namespace boson::shipping {

namespace {

std::wstring lower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& ch : out) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return out;
}

} // namespace

AuthenticodeResult verify_authenticode(const std::filesystem::path& file, std::wstring_view required_subject) {
    AuthenticodeResult result;
    const std::wstring path = file.wstring();

    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path.c_str();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file_info;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    result.status = status;
    // Offline revocation servers are not a reason to reject an otherwise valid signature.
    result.trusted = status == ERROR_SUCCESS ||
                     status == CERT_E_REVOCATION_FAILURE ||
                     status == static_cast<LONG>(CRYPT_E_REVOCATION_OFFLINE) ||
                     status == static_cast<LONG>(CRYPT_E_NO_REVOCATION_CHECK);
    if (!result.trusted) {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "WinVerifyTrust failed: 0x%08lX", static_cast<unsigned long>(status));
        result.error = buffer;
    }

    if (data.hWVTStateData != nullptr) {
        if (CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData)) {
            if (CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0)) {
                if (signer->csCertChain > 0 && signer->pasCertChain != nullptr && signer->pasCertChain[0].pCert != nullptr) {
                    PCCERT_CONTEXT cert = signer->pasCertChain[0].pCert;
                    const DWORD len = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
                    if (len > 1) {
                        std::wstring name(len, L'\0');
                        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), len);
                        name.resize(len - 1);
                        result.signer = std::move(name);
                    }
                }
            }
        }
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    }

    if (!required_subject.empty()) {
        result.subject_matches = !result.signer.empty() && lower(result.signer).find(lower(required_subject)) != std::wstring::npos;
        if (!result.subject_matches && result.error.empty()) {
            result.error = "signer \"" + core::wide_to_utf8(result.signer) + "\" does not match the required subject";
        }
    }
    return result;
}

} // namespace boson::shipping
