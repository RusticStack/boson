#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace boson::shipping {

struct AuthenticodeResult {
    bool trusted = false;       ///< WinVerifyTrust accepted the embedded signature (chain to a trusted root).
    bool subject_matches = true;
    std::wstring signer;        ///< Simple display name of the leaf signer certificate.
    long status = 0;            ///< WinVerifyTrust result (HRESULT / TRUST_E_*).
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return trusted && subject_matches; }
};

/// @brief Verifies the embedded Authenticode signature of an .exe / .msi / .dll / .msix with WinVerifyTrust
/// (WINTRUST_ACTION_GENERIC_VERIFY_V2, no UI, revocation checked from cache only so it never blocks on the
/// network). When required_subject is not empty, the signer's display name must contain it
/// (case-insensitive), e.g. L"Microsoft Corporation".
[[nodiscard]] AuthenticodeResult verify_authenticode(const std::filesystem::path& file, std::wstring_view required_subject = {});

} // namespace boson::shipping
