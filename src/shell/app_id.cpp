#include <boson/shell/app_id.hpp>

#include <boson/core/result.hpp>

#include "shell_util.hpp"

#include <shellapi.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

namespace boson::shell {

bool is_valid_app_user_model_id(std::wstring_view app_id) noexcept {
    if (app_id.empty() || app_id.size() > 128) {
        return false;
    }
    for (const wchar_t ch : app_id) {
        if (ch == L' ' || ch == L'\t' || ch == L'\0') {
            return false;
        }
    }
    return true;
}

bool set_app_user_model_id(std::wstring_view app_id) noexcept {
    if (!is_valid_app_user_model_id(app_id)) {
        return false;
    }
    const std::wstring id(app_id);
    return core::hr_check(::SetCurrentProcessExplicitAppUserModelID(id.c_str()), "SetCurrentProcessExplicitAppUserModelID");
}

std::optional<std::wstring> app_user_model_id() {
    PWSTR id = nullptr;
    if (FAILED(::GetCurrentProcessExplicitAppUserModelID(&id)) || id == nullptr) {
        return std::nullopt;
    }
    std::wstring out(id);
    ::CoTaskMemFree(id);
    return out;
}

bool set_window_app_user_model_id(HWND hwnd, std::wstring_view app_id) noexcept {
    if (hwnd == nullptr || (!app_id.empty() && !is_valid_app_user_model_id(app_id))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    if (!core::hr_check(::SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)), "SHGetPropertyStoreForWindow")) {
        return false;
    }
    HRESULT hr = S_OK;
    if (app_id.empty()) {
        PROPVARIANT empty{};
        ::PropVariantInit(&empty);
        hr = store->SetValue(detail::kPkeyAppUserModelId, empty);
    } else {
        const detail::PropString value(app_id);
        if (!value.ok()) {
            return false;
        }
        hr = store->SetValue(detail::kPkeyAppUserModelId, value.get());
    }
    if (!core::hr_check(hr, "IPropertyStore::SetValue(AppUserModel.ID)")) {
        return false;
    }
    return core::hr_check(store->Commit(), "IPropertyStore::Commit");
}

} // namespace boson::shell
