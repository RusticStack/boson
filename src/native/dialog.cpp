#include <boson/native/dialog.hpp>

#include <boson/core/result.hpp>

#include <shobjidl.h>
#include <wrl/client.h>

#include <memory>

namespace boson::native {

namespace {

using Microsoft::WRL::ComPtr;

struct CoTaskMemDeleter {
    void operator()(wchar_t* ptr) const noexcept {
        if (ptr != nullptr) {
            CoTaskMemFree(ptr);
        }
    }
};

using UniqueCoTaskWString = std::unique_ptr<wchar_t, CoTaskMemDeleter>;

} // namespace

FileDialogResult open_file_dialog(HWND owner, const FileDialogOptions& options) {
    FileDialogResult result;

    ComPtr<IFileOpenDialog> dialog;
    const HRESULT hr_create = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (!core::hr_check(hr_create, "CoCreateInstance(CLSID_FileOpenDialog)")) {
        return result;
    }

    DWORD flags = 0;
    if (BOSON_HR_CHECK(dialog->GetOptions(&flags))) {
        flags |= FOS_FORCEFILESYSTEM;
        if (options.pick_folder) {
            flags |= FOS_PICKFOLDERS;
        }
        if (options.allow_multiple) {
            flags |= FOS_ALLOWMULTISELECT;
        }
        BOSON_HR_CHECK(dialog->SetOptions(flags));
    }

    if (!options.title.empty()) {
        const std::wstring wtitle = core::utf8_to_wide(options.title);
        BOSON_HR_CHECK(dialog->SetTitle(wtitle.c_str()));
    }

    const HRESULT hr_show = dialog->Show(owner);
    if (hr_show == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return result;
    }
    if (!core::hr_check(hr_show, "IFileOpenDialog::Show")) {
        return result;
    }

    ComPtr<IShellItemArray> items;
    if (!BOSON_HR_CHECK(dialog->GetResults(&items)) || !items) {
        return result;
    }

    DWORD count = 0;
    if (!BOSON_HR_CHECK(items->GetCount(&count))) {
        return result;
    }

    result.paths.reserve(static_cast<std::size_t>(count));
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
            wchar_t* raw_path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) && raw_path != nullptr) {
                UniqueCoTaskWString guard(raw_path);
                result.paths.push_back(core::wide_to_utf8(raw_path));
            }
        }
    }

    result.cancelled = result.paths.empty();
    return result;
}

FileDialogResult save_file_dialog(HWND owner, const FileDialogOptions& options) {
    FileDialogResult result;

    ComPtr<IFileSaveDialog> dialog;
    const HRESULT hr_create = CoCreateInstance(
        CLSID_FileSaveDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (!core::hr_check(hr_create, "CoCreateInstance(CLSID_FileSaveDialog)")) {
        return result;
    }

    DWORD flags = 0;
    if (BOSON_HR_CHECK(dialog->GetOptions(&flags))) {
        flags |= FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT;
        BOSON_HR_CHECK(dialog->SetOptions(flags));
    }

    if (!options.title.empty()) {
        const std::wstring wtitle = core::utf8_to_wide(options.title);
        BOSON_HR_CHECK(dialog->SetTitle(wtitle.c_str()));
    }
    if (!options.default_name.empty()) {
        const std::wstring wname = core::utf8_to_wide(options.default_name);
        BOSON_HR_CHECK(dialog->SetFileName(wname.c_str()));
    }

    const HRESULT hr_show = dialog->Show(owner);
    if (hr_show == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return result;
    }
    if (!core::hr_check(hr_show, "IFileSaveDialog::Show")) {
        return result;
    }

    ComPtr<IShellItem> item;
    if (!BOSON_HR_CHECK(dialog->GetResult(&item)) || !item) {
        return result;
    }

    wchar_t* raw_path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) && raw_path != nullptr) {
        UniqueCoTaskWString guard(raw_path);
        result.paths.push_back(core::wide_to_utf8(raw_path));
        result.cancelled = false;
    }

    return result;
}

} // namespace boson::native
