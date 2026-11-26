#include <boson/native/dialog.hpp>

#include <boson/core/result.hpp>

#include "../desktop/desktop_util.hpp"

#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace boson::native {

namespace {

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

struct CoTaskMemDeleter {
    void operator()(wchar_t* ptr) const noexcept {
        if (ptr != nullptr) {
            CoTaskMemFree(ptr);
        }
    }
};

using UniqueCoTaskWString = std::unique_ptr<wchar_t, CoTaskMemDeleter>;

/// Fires the on_shown hook once, from the first OnFolderChange (the dialog window exists by then).
class DialogEvents : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IFileDialogEvents> {
public:
    explicit DialogEvents(std::function<void(IFileDialog&, HWND)> hook) : m_hook(std::move(hook)) {}

    IFACEMETHODIMP OnFileOk(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP OnFolderChange(IFileDialog* dialog) override {
        if (m_fired || !m_hook || dialog == nullptr) {
            return S_OK;
        }
        m_fired = true;
        HWND hwnd = nullptr;
        ComPtr<IOleWindow> window;
        if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&window))) && window) {
            window->GetWindow(&hwnd);
        }
        try {
            m_hook(*dialog, hwnd);
        } catch (...) {
            // Never unwind into the shell's COM caller.
        }
        return S_OK;
    }
    IFACEMETHODIMP OnSelectionChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override { return S_OK; }
    IFACEMETHODIMP OnTypeChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override { return S_OK; }

private:
    std::function<void(IFileDialog&, HWND)> m_hook;
    bool m_fired = false;
};

[[nodiscard]] std::wstring_view strip_extension_prefix(std::wstring_view ext) noexcept {
    if (ext.starts_with(L"*.")) {
        ext.remove_prefix(2);
    } else if (ext.starts_with(L'.')) {
        ext.remove_prefix(1);
    }
    return ext;
}

[[nodiscard]] std::optional<std::string> path_of(IShellItem* item) {
    wchar_t* raw_path = nullptr;
    if (item == nullptr || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) || raw_path == nullptr) {
        return std::nullopt;
    }
    UniqueCoTaskWString guard(raw_path);
    return core::wide_to_utf8(raw_path);
}

[[nodiscard]] ComPtr<IShellItem> folder_item(const std::wstring& path) {
    ComPtr<IShellItem> item;
    if (path.empty() || FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return nullptr;
    }
    return item;
}

/// Applies the options shared by open, save and folder dialogs. filter_storage keeps the specs alive.
void configure(IFileDialog& dialog, FileDialogKind kind, const FileDialogOptions& options,
               std::vector<std::wstring>& filter_storage) {
    DWORD flags = 0;
    if (BOSON_HR_CHECK(dialog.GetOptions(&flags))) {
        flags |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        if (options.path_must_exist) {
            flags |= FOS_PATHMUSTEXIST;
        } else {
            flags &= ~static_cast<DWORD>(FOS_PATHMUSTEXIST);
        }
        if (kind == FileDialogKind::Folder) {
            flags |= FOS_PICKFOLDERS;
        }
        if (kind != FileDialogKind::Save) {
            flags |= FOS_FILEMUSTEXIST;
            if (options.allow_multiple) {
                flags |= FOS_ALLOWMULTISELECT;
            }
        } else if (options.confirm_overwrite) {
            flags |= FOS_OVERWRITEPROMPT;
        } else {
            flags &= ~static_cast<DWORD>(FOS_OVERWRITEPROMPT);
        }
        if (options.show_hidden) {
            flags |= FOS_FORCESHOWHIDDEN;
        }
        if (!options.add_to_recent) {
            flags |= FOS_DONTADDTORECENT;
        }
        BOSON_HR_CHECK(dialog.SetOptions(flags));
    }

    if (!options.title.empty()) {
        BOSON_HR_CHECK(dialog.SetTitle(core::utf8_to_wide(options.title).c_str()));
    }
    if (!options.button_label.empty()) {
        BOSON_HR_CHECK(dialog.SetOkButtonLabel(core::utf8_to_wide(options.button_label).c_str()));
    }

    std::wstring file_name = core::utf8_to_wide(options.default_name);
    if (!options.default_path.empty()) {
        const std::wstring path = core::utf8_to_wide(options.default_path);
        const DWORD attrs = GetFileAttributesW(path.c_str());
        std::wstring folder;
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            folder = path;
        } else {
            const std::size_t slash = path.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                folder = path.substr(0, slash);
                if (file_name.empty()) {
                    file_name = path.substr(slash + 1);
                }
            } else if (file_name.empty()) {
                file_name = path;
            }
        }
        if (const auto item = folder_item(folder)) {
            BOSON_HR_CHECK(dialog.SetFolder(item.Get()));
        }
    }
    if (!file_name.empty() && kind != FileDialogKind::Folder) {
        BOSON_HR_CHECK(dialog.SetFileName(file_name.c_str()));
    }

    if (kind == FileDialogKind::Folder || options.filters.empty()) {
        if (kind == FileDialogKind::Save && !options.default_extension.empty()) {
            BOSON_HR_CHECK(dialog.SetDefaultExtension(
                std::wstring(strip_extension_prefix(core::utf8_to_wide(options.default_extension))).c_str()));
        }
        return;
    }

    filter_storage.clear();
    filter_storage.reserve(options.filters.size() * 2);
    for (const FileFilter& filter : options.filters) {
        filter_storage.push_back(core::utf8_to_wide(filter.name.empty() ? std::string("Files") : filter.name));
        filter_storage.push_back(filter_spec(filter));
    }
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(options.filters.size());
    for (std::size_t i = 0; i + 1 < filter_storage.size(); i += 2) {
        specs.push_back(COMDLG_FILTERSPEC{filter_storage[i].c_str(), filter_storage[i + 1].c_str()});
    }
    if (BOSON_HR_CHECK(dialog.SetFileTypes(static_cast<UINT>(specs.size()), specs.data()))) {
        const UINT index = options.filter_index < specs.size() ? options.filter_index : 0;
        BOSON_HR_CHECK(dialog.SetFileTypeIndex(index + 1));
    }

    if (kind == FileDialogKind::Save) {
        std::wstring ext = core::utf8_to_wide(options.default_extension);
        if (ext.empty()) {
            const std::size_t index = options.filter_index < options.filters.size() ? options.filter_index : 0;
            for (const std::string& candidate : options.filters[index].extensions) {
                const std::wstring wide = core::utf8_to_wide(candidate);
                const std::wstring_view bare = strip_extension_prefix(wide);
                if (!bare.empty() && bare != L"*") {
                    ext = bare;
                    break;
                }
            }
        }
        if (!ext.empty()) {
            BOSON_HR_CHECK(dialog.SetDefaultExtension(std::wstring(strip_extension_prefix(ext)).c_str()));
        }
    }
}

// ---- defer_to_message_loop -------------------------------------------------------------------------------

constexpr UINT kWmDeferred = WM_APP + 0x51;

struct DeferredQueue {
    std::deque<std::function<void()>> queue;
    std::unique_ptr<desktop::detail::HiddenWindow> window;

    bool ensure_window() {
        if (window == nullptr) {
            window = std::make_unique<desktop::detail::HiddenWindow>(
                [this](UINT msg, WPARAM, LPARAM) -> std::optional<LRESULT> {
                    if (msg != kWmDeferred) {
                        return std::nullopt;
                    }
                    run_one();
                    return 0;
                },
                true);
        }
        return window->hwnd() != nullptr;
    }

    void run_one() {
        if (queue.empty()) {
            return;
        }
        std::function<void()> fn = std::move(queue.front());
        // One posted message per item, so each item starts from its own dispatch.
        queue.pop_front();
        try {
            fn();
        } catch (const std::exception& ex) {
            OutputDebugStringA("[Boson] deferred task threw: ");
            OutputDebugStringA(ex.what());
            OutputDebugStringA("\n");
        } catch (...) {
            OutputDebugStringA("[Boson] deferred task threw a non-standard exception\n");
        }
    }
};

DeferredQueue& deferred_queue() {
    thread_local DeferredQueue state;
    return state;
}

} // namespace

std::wstring filter_spec(const FileFilter& filter) {
    std::wstring spec;
    for (const std::string& ext : filter.extensions) {
        const std::wstring wide = core::utf8_to_wide(ext);
        const std::wstring_view bare = strip_extension_prefix(wide);
        if (bare.empty()) {
            continue;
        }
        if (!spec.empty()) {
            spec.push_back(L';');
        }
        if (bare == L"*") {
            spec += L"*.*";
        } else {
            spec += L"*.";
            spec += bare;
        }
    }
    return spec.empty() ? std::wstring(L"*.*") : spec;
}

FileDialogResult show_file_dialog(FileDialogKind kind, HWND owner, const FileDialogOptions& options) {
    FileDialogResult result;

    ComPtr<IFileDialog> dialog;
    const HRESULT hr_create = CoCreateInstance(
        kind == FileDialogKind::Save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (!core::hr_check(hr_create, "CoCreateInstance(CLSID_FileOpenDialog / CLSID_FileSaveDialog)")) {
        result.hr = hr_create;
        return result;
    }

    std::vector<std::wstring> filter_storage;
    configure(*dialog.Get(), kind, options, filter_storage);

    DWORD cookie = 0;
    bool advised = false;
    if (options.on_shown) {
        const auto events = Make<DialogEvents>(options.on_shown);
        advised = events && BOSON_HR_CHECK(dialog->Advise(events.Get(), &cookie));
    }

    const HRESULT hr_show = dialog->Show(owner);
    if (advised) {
        dialog->Unadvise(cookie);
    }

    UINT type_index = 0;
    if (!options.filters.empty() && SUCCEEDED(dialog->GetFileTypeIndex(&type_index)) && type_index > 0) {
        result.filter_index = type_index - 1;
    }

    if (hr_show == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return result;
    }
    if (!core::hr_check(hr_show, "IFileDialog::Show")) {
        result.hr = hr_show;
        return result;
    }

    if (kind == FileDialogKind::Save) {
        ComPtr<IShellItem> item;
        if (BOSON_HR_CHECK(dialog->GetResult(&item)) && item) {
            if (auto path = path_of(item.Get())) {
                result.paths.push_back(std::move(*path));
            }
        }
    } else {
        ComPtr<IFileOpenDialog> open;
        ComPtr<IShellItemArray> items;
        DWORD count = 0;
        if (SUCCEEDED(dialog.As(&open)) && BOSON_HR_CHECK(open->GetResults(&items)) && items &&
            BOSON_HR_CHECK(items->GetCount(&count))) {
            result.paths.reserve(static_cast<std::size_t>(count));
            for (DWORD i = 0; i < count; ++i) {
                ComPtr<IShellItem> item;
                if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
                    if (auto path = path_of(item.Get())) {
                        result.paths.push_back(std::move(*path));
                    }
                }
            }
        }
    }

    result.cancelled = result.paths.empty();
    return result;
}

FileDialogResult open_file_dialog(HWND owner, const FileDialogOptions& options) {
    return show_file_dialog(options.pick_folder ? FileDialogKind::Folder : FileDialogKind::Open, owner, options);
}

FileDialogResult save_file_dialog(HWND owner, const FileDialogOptions& options) {
    return show_file_dialog(FileDialogKind::Save, owner, options);
}

FileDialogResult pick_folder_dialog(HWND owner, const FileDialogOptions& options) {
    return show_file_dialog(FileDialogKind::Folder, owner, options);
}

void show_file_dialog_async(
    FileDialogKind kind,
    HWND owner,
    FileDialogOptions options,
    std::function<void(FileDialogResult)> done) {
    defer_to_message_loop([kind, owner, options = std::move(options), done = std::move(done)]() {
        FileDialogResult result = show_file_dialog(kind, owner, options);
        if (done) {
            done(std::move(result));
        }
    });
}

void defer_to_message_loop(std::function<void()> fn) {
    if (!fn) {
        return;
    }
    DeferredQueue& state = deferred_queue();
    state.queue.push_back(std::move(fn));
    if (!state.ensure_window() || PostMessageW(state.window->hwnd(), kWmDeferred, 0, 0) == FALSE) {
        // No window (or a full message queue): run now rather than never.
        state.run_one();
    }
}

} // namespace boson::native
