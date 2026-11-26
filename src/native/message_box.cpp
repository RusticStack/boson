#include <boson/native/message_box.hpp>

#include <boson/core/result.hpp>
#include <boson/native/dialog.hpp>

#include <commctrl.h>

#include <array>
#include <utility>

namespace boson::native {

namespace {

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

/// comctl32 v6 loaded through shell32's common-controls manifest (resource 124), so TaskDialogIndirect works
/// without a manifest dependency in the executable. Loaded once, kept for the process lifetime.
struct TaskDialogApi {
    HANDLE actctx = INVALID_HANDLE_VALUE;
    TaskDialogIndirectFn fn = nullptr;

    TaskDialogApi() noexcept {
        std::array<wchar_t, MAX_PATH> system_dir{};
        const UINT len = GetSystemDirectoryW(system_dir.data(), static_cast<UINT>(system_dir.size()));
        if (len == 0 || len >= system_dir.size()) {
            return;
        }
        std::wstring shell32{system_dir.data(), len};
        shell32 += L"\\shell32.dll";

        ACTCTXW ctx{};
        ctx.cbSize = sizeof(ctx);
        ctx.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID;
        ctx.lpSource = shell32.c_str();
        ctx.lpAssemblyDirectory = system_dir.data();
        ctx.lpResourceName = reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(124));
        actctx = CreateActCtxW(&ctx);

        ULONG_PTR cookie = 0;
        const bool activated = actctx != INVALID_HANDLE_VALUE && ActivateActCtx(actctx, &cookie) != FALSE;
        if (HMODULE comctl = LoadLibraryW(L"comctl32.dll"); comctl != nullptr) {
            fn = reinterpret_cast<TaskDialogIndirectFn>(reinterpret_cast<void*>(GetProcAddress(comctl, "TaskDialogIndirect")));
        }
        if (activated) {
            DeactivateActCtx(0, cookie);
        }
    }
};

const TaskDialogApi& task_dialog_api() noexcept {
    static const TaskDialogApi api;
    return api;
}

/// Activates the comctl32 v6 context for the lifetime of the object (window classes resolve to v6).
class ScopedActivation {
public:
    explicit ScopedActivation(HANDLE ctx) noexcept {
        if (ctx != INVALID_HANDLE_VALUE) {
            m_active = ActivateActCtx(ctx, &m_cookie) != FALSE;
        }
    }
    ~ScopedActivation() {
        if (m_active) {
            DeactivateActCtx(0, m_cookie);
        }
    }
    ScopedActivation(const ScopedActivation&) = delete;
    ScopedActivation& operator=(const ScopedActivation&) = delete;

private:
    ULONG_PTR m_cookie = 0;
    bool m_active = false;
};

[[nodiscard]] bool label_is(std::string_view label, std::string_view word) noexcept {
    std::size_t j = 0;
    for (const char raw : label) {
        if (raw == '&') {
            continue;
        }
        const char ch = (raw >= 'A' && raw <= 'Z') ? static_cast<char>(raw - 'A' + 'a') : raw;
        if (j >= word.size() || ch != word[j]) {
            return false;
        }
        ++j;
    }
    return j == word.size();
}

HRESULT CALLBACK task_dialog_callback(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR ref_data) {
    const auto* options = reinterpret_cast<const MessageBoxOptions*>(ref_data);
    if (msg == TDN_CREATED && options != nullptr && options->on_shown) {
        try {
            options->on_shown(hwnd);
        } catch (...) {
            // Never unwind into comctl32.
        }
    }
    return S_OK;
}

[[nodiscard]] std::vector<std::string> effective_buttons(const MessageBoxOptions& options) {
    if (options.buttons.empty()) {
        return {"OK"};
    }
    return options.buttons;
}

MessageBoxResult fallback_message_box(HWND owner, const MessageBoxOptions& options) {
    const std::vector<std::string> buttons = effective_buttons(options);
    const int cancel = resolve_cancel_id(options);
    UINT type = MB_OK;
    const bool yes_no = buttons.size() >= 2 && label_is(buttons[0], "yes") && label_is(buttons[1], "no");
    if (buttons.size() == 2) {
        type = yes_no ? MB_YESNO : MB_OKCANCEL;
    } else if (buttons.size() >= 3) {
        type = MB_YESNOCANCEL;
    }
    switch (options.icon) {
    case MessageBoxIcon::Info: type |= MB_ICONINFORMATION; break;
    case MessageBoxIcon::Warning:
    case MessageBoxIcon::Shield: type |= MB_ICONWARNING; break;
    case MessageBoxIcon::Error: type |= MB_ICONERROR; break;
    case MessageBoxIcon::Question: type |= MB_ICONQUESTION; break;
    case MessageBoxIcon::None: break;
    }
    if (options.default_id == 1) {
        type |= MB_DEFBUTTON2;
    } else if (options.default_id == 2) {
        type |= MB_DEFBUTTON3;
    }
    std::string text = options.message;
    if (!options.detail.empty()) {
        if (!text.empty()) {
            text += "\n\n";
        }
        text += options.detail;
    }
    const int id = MessageBoxW(owner, core::utf8_to_wide(text).c_str(), core::utf8_to_wide(options.title).c_str(), type);
    MessageBoxResult result;
    result.used_fallback = true;
    result.checkbox_checked = options.checkbox_checked;
    switch (id) {
    case IDOK:
    case IDYES: result.response = 0; break;
    case IDNO: result.response = 1; break;
    case IDCANCEL:
        result.cancelled = true;
        result.response = buttons.size() >= 3 ? 2 : (buttons.size() == 2 ? 1 : cancel);
        break;
    default:
        result.hr = HRESULT_FROM_WIN32(GetLastError());
        result.cancelled = true;
        result.response = cancel;
        break;
    }
    return result;
}

} // namespace

std::optional<MessageBoxIcon> parse_message_box_icon(std::string_view name) noexcept {
    if (name.empty() || name == "none") return MessageBoxIcon::None;
    if (name == "info" || name == "information") return MessageBoxIcon::Info;
    if (name == "warning" || name == "warn") return MessageBoxIcon::Warning;
    if (name == "error") return MessageBoxIcon::Error;
    if (name == "question") return MessageBoxIcon::Question;
    if (name == "shield") return MessageBoxIcon::Shield;
    return std::nullopt;
}

int resolve_cancel_id(const MessageBoxOptions& options) noexcept {
    const std::size_t count = options.buttons.empty() ? 1 : options.buttons.size();
    if (options.cancel_id >= 0 && static_cast<std::size_t>(options.cancel_id) < count) {
        return options.cancel_id;
    }
    for (std::size_t i = 0; i < options.buttons.size(); ++i) {
        if (label_is(options.buttons[i], "cancel") || label_is(options.buttons[i], "no")) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

bool task_dialog_available() noexcept {
    return task_dialog_api().fn != nullptr;
}

MessageBoxResult show_message_box(HWND owner, const MessageBoxOptions& options) {
    const TaskDialogApi& api = task_dialog_api();
    if (api.fn == nullptr) {
        return fallback_message_box(owner, options);
    }

    const std::vector<std::string> labels = effective_buttons(options);
    std::vector<std::wstring> wide_labels;
    wide_labels.reserve(labels.size());
    for (const std::string& label : labels) {
        wide_labels.push_back(core::utf8_to_wide(label));
    }
    std::vector<TASKDIALOG_BUTTON> buttons;
    buttons.reserve(labels.size());
    for (std::size_t i = 0; i < wide_labels.size(); ++i) {
        buttons.push_back(TASKDIALOG_BUTTON{kMessageBoxButtonBaseId + static_cast<int>(i), wide_labels[i].c_str()});
    }

    const std::wstring title = core::utf8_to_wide(options.title);
    const std::wstring message = core::utf8_to_wide(options.message);
    const std::wstring detail = core::utf8_to_wide(options.detail);
    const std::wstring checkbox = core::utf8_to_wide(options.checkbox_label);
    const std::wstring footer = core::utf8_to_wide(options.footer);

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.hInstance = GetModuleHandleW(nullptr);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    if (options.command_links) {
        config.dwFlags |= TDF_USE_COMMAND_LINKS;
    }
    if (options.checkbox_checked) {
        config.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
    }
    config.pszWindowTitle = title.empty() ? nullptr : title.c_str();
    config.pszMainInstruction = message.empty() ? nullptr : message.c_str();
    config.pszContent = detail.empty() ? nullptr : detail.c_str();
    config.pszVerificationText = checkbox.empty() ? nullptr : checkbox.c_str();
    config.pszFooter = footer.empty() ? nullptr : footer.c_str();
    config.cButtons = static_cast<UINT>(buttons.size());
    config.pButtons = buttons.data();
    const int default_index = options.default_id >= 0 && static_cast<std::size_t>(options.default_id) < buttons.size()
                                  ? options.default_id
                                  : 0;
    config.nDefaultButton = kMessageBoxButtonBaseId + default_index;
    switch (options.icon) {
    case MessageBoxIcon::Info: config.pszMainIcon = TD_INFORMATION_ICON; break;
    case MessageBoxIcon::Warning: config.pszMainIcon = TD_WARNING_ICON; break;
    case MessageBoxIcon::Error: config.pszMainIcon = TD_ERROR_ICON; break;
    case MessageBoxIcon::Shield: config.pszMainIcon = TD_SHIELD_ICON; break;
    case MessageBoxIcon::Question:
        config.dwFlags |= TDF_USE_HICON_MAIN;
        config.hMainIcon = LoadIconW(nullptr, IDI_QUESTION);
        break;
    case MessageBoxIcon::None: break;
    }
    config.pfCallback = &task_dialog_callback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&options);

    int pressed = 0;
    BOOL verified = options.checkbox_checked ? TRUE : FALSE;
    HRESULT hr = E_FAIL;
    {
        const ScopedActivation activation(api.actctx);
        hr = api.fn(&config, &pressed, nullptr, &verified);
    }

    MessageBoxResult result;
    result.hr = hr;
    result.checkbox_checked = verified != FALSE;
    if (!core::hr_check(hr, "TaskDialogIndirect")) {
        return fallback_message_box(owner, options);
    }
    if (pressed >= kMessageBoxButtonBaseId && pressed < kMessageBoxButtonBaseId + static_cast<int>(buttons.size())) {
        result.response = pressed - kMessageBoxButtonBaseId;
    } else {
        result.cancelled = true;
        result.response = resolve_cancel_id(options);
    }
    return result;
}

void show_message_box_async(HWND owner, MessageBoxOptions options, std::function<void(MessageBoxResult)> done) {
    defer_to_message_loop([owner, options = std::move(options), done = std::move(done)]() {
        MessageBoxResult result = show_message_box(owner, options);
        if (done) {
            done(std::move(result));
        }
    });
}

void show_error_box(HWND owner, std::string_view title, std::string_view content) {
    MessageBoxOptions options;
    options.title = std::string(title);
    options.message = std::string(title);
    options.detail = std::string(content);
    options.icon = MessageBoxIcon::Error;
    (void)show_message_box(owner, options);
}

} // namespace boson::native
