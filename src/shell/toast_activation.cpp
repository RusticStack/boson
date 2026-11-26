#include <boson/shell/toast_activation.hpp>

#include <boson/app/lifecycle.hpp>
#include <boson/core/result.hpp>
#include <boson/shell/app_id.hpp>

#include "shell_util.hpp"

#include <NotificationActivationCallback.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <filesystem>
#include <mutex>

namespace boson::shell {

namespace {

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

class RegKey {
public:
    RegKey() noexcept = default;
    ~RegKey() {
        if (m_key != nullptr) {
            ::RegCloseKey(m_key);
        }
    }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    HKEY* put() noexcept { return &m_key; }
    [[nodiscard]] HKEY get() const noexcept { return m_key; }

private:
    HKEY m_key = nullptr;
};

bool set_string(HKEY key, const wchar_t* name, std::wstring_view value) noexcept {
    const std::wstring copy(value);
    return ::RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(copy.c_str()),
                            static_cast<DWORD>((copy.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

std::optional<std::wstring> get_string(HKEY root, const std::wstring& subkey, const wchar_t* name) {
    DWORD bytes = 0;
    if (::RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) {
        return std::nullopt;
    }
    std::wstring out(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, out.data(), &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    out.resize(wcsnlen(out.c_str(), out.size()));
    return out;
}

std::wstring clsid_key(const GUID& clsid) {
    return L"Software\\Classes\\CLSID\\" + guid_to_string(clsid);
}

std::wstring app_id_key(std::wstring_view app_id) {
    return L"Software\\Classes\\AppUserModelId\\" + std::wstring(app_id);
}

std::optional<std::filesystem::path> programs_folder() {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, &raw)) || raw == nullptr) {
        if (raw != nullptr) {
            ::CoTaskMemFree(raw);
        }
        return std::nullopt;
    }
    std::filesystem::path out(raw);
    ::CoTaskMemFree(raw);
    return out;
}

bool is_safe_component(std::wstring_view part) noexcept {
    if (part.empty()) {
        return false;
    }
    if (part == L"." || part == L"..") {
        return false;
    }
    return part.find_first_of(L"<>:\"|?*") == std::wstring_view::npos;
}

} // namespace

// ---- GUID / argv ----------------------------------------------------------------------------------------

std::optional<GUID> parse_guid(std::wstring_view text) noexcept {
    std::wstring braced;
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.front() == L'{') {
        braced.assign(text);
    } else {
        braced = L"{" + std::wstring(text) + L"}";
    }
    GUID guid{};
    if (FAILED(::IIDFromString(braced.c_str(), &guid))) {
        return std::nullopt;
    }
    return guid;
}

std::wstring guid_to_string(const GUID& guid) {
    wchar_t buffer[64]{};
    const int length = ::StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    return length > 1 ? std::wstring(buffer, static_cast<std::size_t>(length - 1)) : std::wstring{};
}

bool was_launched_by_toast(std::span<const std::wstring> args) noexcept {
    for (const auto& arg : args) {
        if (arg.size() == kToastActivatedSwitch.size() &&
            ::CompareStringOrdinal(arg.c_str(), static_cast<int>(arg.size()), kToastActivatedSwitch.data(),
                                   static_cast<int>(kToastActivatedSwitch.size()), TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

// ---- ToastActivator --------------------------------------------------------------------------------------

struct ToastActivator::State {
    std::mutex mutex;
    Handler handler;
};

namespace {

class ActivationCallback final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, INotificationActivationCallback> {
public:
    explicit ActivationCallback(std::weak_ptr<ToastActivator::State> state) : m_state(std::move(state)) {}

    IFACEMETHODIMP Activate(LPCWSTR app_user_model_id, LPCWSTR invoked_args,
                            const NOTIFICATION_USER_INPUT_DATA* data, ULONG count) override {
        try {
            const auto state = m_state.lock();
            if (state == nullptr) {
                return S_OK;
            }
            ToastActivation activation;
            if (app_user_model_id != nullptr) {
                activation.app_user_model_id = app_user_model_id;
            }
            if (invoked_args != nullptr) {
                activation.arguments = core::wide_to_utf8(invoked_args);
            }
            for (ULONG i = 0; data != nullptr && i < count; ++i) {
                const std::string key = data[i].Key != nullptr ? core::wide_to_utf8(data[i].Key) : std::string{};
                activation.user_input[key] = data[i].Value != nullptr ? core::wide_to_utf8(data[i].Value) : std::string{};
            }
            ToastActivator::Handler handler;
            {
                std::scoped_lock lock(state->mutex);
                handler = state->handler;
            }
            if (handler) {
                handler(std::move(activation));
            }
            return S_OK;
        } catch (...) {
            return E_FAIL; // never unwind into COM
        }
    }

private:
    std::weak_ptr<ToastActivator::State> m_state;
};

class ActivationFactory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    explicit ActivationFactory(std::weak_ptr<ToastActivator::State> state) : m_state(std::move(state)) {}

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        const auto callback = Make<ActivationCallback>(m_state);
        if (callback == nullptr) {
            return E_OUTOFMEMORY;
        }
        return callback.CopyTo(riid, object);
    }

    IFACEMETHODIMP LockServer(BOOL /*lock*/) override { return S_OK; }

private:
    std::weak_ptr<ToastActivator::State> m_state;
};

} // namespace

ToastActivator::ToastActivator(const GUID& clsid, Handler handler)
    : m_clsid(clsid), m_state(std::make_shared<State>()) {
    m_state->handler = std::move(handler);
    const auto factory = Make<ActivationFactory>(std::weak_ptr<State>(m_state));
    if (factory == nullptr) {
        m_hr = E_OUTOFMEMORY;
        return;
    }
    m_hr = ::CoRegisterClassObject(m_clsid, factory.Get(), CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &m_cookie);
    if (!core::hr_check(m_hr, "CoRegisterClassObject(ToastActivator)")) {
        m_cookie = 0;
    }
}

ToastActivator::~ToastActivator() {
    if (m_cookie != 0) {
        ::CoRevokeClassObject(m_cookie);
    }
    std::scoped_lock lock(m_state->mutex);
    m_state->handler = nullptr;
}

void ToastActivator::set_handler(Handler handler) {
    std::scoped_lock lock(m_state->mutex);
    m_state->handler = std::move(handler);
}

// ---- Registry --------------------------------------------------------------------------------------------

bool register_toast_com_server(const GUID& clsid, std::wstring_view executable, std::wstring_view arguments) {
    const std::wstring exe = executable.empty() ? app::executable_path() : std::wstring(executable);
    if (exe.empty()) {
        return false;
    }
    std::wstring command = app::quote_argument(exe);
    if (!arguments.empty()) {
        command += L' ';
        command += arguments;
    }
    RegKey key;
    const std::wstring path = clsid_key(clsid) + L"\\LocalServer32";
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, key.put(), nullptr) != ERROR_SUCCESS) {
        return false;
    }
    return set_string(key.get(), nullptr, command);
}

bool unregister_toast_com_server(const GUID& clsid) {
    const std::wstring path = clsid_key(clsid);
    const LSTATUS status = ::RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return false;
    }
    const LSTATUS leaf = ::RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
    return leaf == ERROR_SUCCESS || leaf == ERROR_FILE_NOT_FOUND;
}

std::optional<std::wstring> toast_com_server_command(const GUID& clsid) {
    return get_string(HKEY_CURRENT_USER, clsid_key(clsid) + L"\\LocalServer32", nullptr);
}

bool register_app_user_model_id(const AppIdRegistration& registration) {
    if (!is_valid_app_user_model_id(registration.app_user_model_id) || registration.display_name.empty()) {
        return false;
    }
    RegKey key;
    const std::wstring path = app_id_key(registration.app_user_model_id);
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, key.put(), nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = set_string(key.get(), L"DisplayName", registration.display_name);
    if (!registration.icon_path.empty()) {
        ok = set_string(key.get(), L"IconUri", registration.icon_path) && ok;
    } else {
        ::RegDeleteValueW(key.get(), L"IconUri");
    }
    if (!registration.icon_background_color.empty()) {
        ok = set_string(key.get(), L"IconBackgroundColor", registration.icon_background_color) && ok;
    }
    if (registration.activator_clsid.has_value()) {
        ok = set_string(key.get(), L"CustomActivator", guid_to_string(*registration.activator_clsid)) && ok;
    } else {
        ::RegDeleteValueW(key.get(), L"CustomActivator");
    }
    return ok;
}

bool unregister_app_user_model_id(std::wstring_view app_id) {
    if (!is_valid_app_user_model_id(app_id)) {
        return false;
    }
    const std::wstring path = app_id_key(app_id);
    const LSTATUS status = ::RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return false;
    }
    const LSTATUS leaf = ::RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
    return leaf == ERROR_SUCCESS || leaf == ERROR_FILE_NOT_FOUND;
}

bool is_app_user_model_id_registered(std::wstring_view app_id) {
    if (!is_valid_app_user_model_id(app_id)) {
        return false;
    }
    return get_string(HKEY_CURRENT_USER, app_id_key(app_id), L"DisplayName").has_value();
}

// ---- Start-menu shortcut ---------------------------------------------------------------------------------

std::optional<std::wstring> start_menu_shortcut_path(std::wstring_view name, std::wstring_view folder) {
    if (!is_safe_component(name) || name.find_first_of(L"\\/") != std::wstring_view::npos) {
        return std::nullopt;
    }
    const auto programs = programs_folder();
    if (!programs.has_value()) {
        return std::nullopt;
    }
    std::filesystem::path path = *programs;
    if (!folder.empty()) {
        if (!is_safe_component(folder) || folder.find(L"..") != std::wstring_view::npos) {
            return std::nullopt;
        }
        path /= std::filesystem::path(folder);
    }
    path /= std::wstring(name) + L".lnk";
    return path.wstring();
}

bool create_start_menu_shortcut(const StartMenuShortcut& shortcut) {
    if (!is_valid_app_user_model_id(shortcut.app_user_model_id)) {
        return false;
    }
    const auto path = start_menu_shortcut_path(shortcut.name, shortcut.folder);
    if (!path.has_value()) {
        return false;
    }
    const detail::ScopedCom com;
    if (!com.ok()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(*path).parent_path(), ec);
    if (ec) {
        return false;
    }

    ComPtr<IShellLinkW> link;
    if (!BOSON_HR_CHECK(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        return false;
    }
    const std::wstring target = shortcut.target.empty() ? app::executable_path() : shortcut.target;
    if (!BOSON_HR_CHECK(link->SetPath(target.c_str()))) {
        return false;
    }
    if (!shortcut.arguments.empty()) {
        BOSON_HR_CHECK(link->SetArguments(shortcut.arguments.c_str()));
    }
    if (!shortcut.description.empty()) {
        BOSON_HR_CHECK(link->SetDescription(shortcut.description.c_str()));
    }
    const std::wstring working_dir = shortcut.working_directory.empty()
        ? std::filesystem::path(target).parent_path().wstring()
        : shortcut.working_directory;
    BOSON_HR_CHECK(link->SetWorkingDirectory(working_dir.c_str()));
    const std::wstring& icon = shortcut.icon_path.empty() ? target : shortcut.icon_path;
    BOSON_HR_CHECK(link->SetIconLocation(icon.c_str(), shortcut.icon_index));

    ComPtr<IPropertyStore> store;
    if (!BOSON_HR_CHECK(link.As(&store))) {
        return false;
    }
    const detail::PropString app_id(shortcut.app_user_model_id);
    if (!app_id.ok() || !BOSON_HR_CHECK(store->SetValue(detail::kPkeyAppUserModelId, app_id.get()))) {
        return false;
    }
    if (shortcut.toast_activator_clsid.has_value()) {
        PROPVARIANT clsid{};
        ::PropVariantInit(&clsid);
        clsid.puuid = static_cast<CLSID*>(::CoTaskMemAlloc(sizeof(CLSID)));
        if (clsid.puuid == nullptr) {
            return false;
        }
        *clsid.puuid = *shortcut.toast_activator_clsid;
        clsid.vt = VT_CLSID;
        const HRESULT hr = store->SetValue(detail::kPkeyToastActivatorClsid, clsid);
        ::PropVariantClear(&clsid);
        if (!core::hr_check(hr, "IPropertyStore::SetValue(ToastActivatorCLSID)")) {
            return false;
        }
    }
    if (!BOSON_HR_CHECK(store->Commit())) {
        return false;
    }
    ComPtr<IPersistFile> file;
    if (!BOSON_HR_CHECK(link.As(&file))) {
        return false;
    }
    return core::hr_check(file->Save(path->c_str(), TRUE), "IPersistFile::Save(shortcut)");
}

bool remove_start_menu_shortcut(std::wstring_view name, std::wstring_view folder) {
    const auto path = start_menu_shortcut_path(name, folder);
    if (!path.has_value()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(*path, ec);
    if (ec) {
        return false;
    }
    if (!folder.empty()) {
        const auto dir = std::filesystem::path(*path).parent_path();
        if (std::filesystem::is_empty(dir, ec) && !ec) {
            std::filesystem::remove(dir, ec);
        }
    }
    return true;
}

std::optional<ShortcutInfo> read_shortcut(std::wstring_view lnk_path) {
    const detail::ScopedCom com;
    if (!com.ok()) {
        return std::nullopt;
    }
    ComPtr<IShellLinkW> link;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        return std::nullopt;
    }
    ComPtr<IPersistFile> file;
    const std::wstring path(lnk_path);
    if (FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(), STGM_READ))) {
        return std::nullopt;
    }
    ShortcutInfo info;
    wchar_t buffer[MAX_PATH]{};
    if (SUCCEEDED(link->GetPath(buffer, MAX_PATH, nullptr, SLGP_RAWPATH))) {
        info.target = buffer;
    }
    wchar_t args[INFOTIPSIZE]{};
    if (SUCCEEDED(link->GetArguments(args, INFOTIPSIZE))) {
        info.arguments = args;
    }
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(link.As(&store))) {
        PROPVARIANT value{};
        ::PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(detail::kPkeyAppUserModelId, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
            info.app_user_model_id = value.pwszVal;
        }
        ::PropVariantClear(&value);
        if (SUCCEEDED(store->GetValue(detail::kPkeyToastActivatorClsid, &value)) && value.vt == VT_CLSID && value.puuid != nullptr) {
            info.toast_activator_clsid = *value.puuid;
        }
        ::PropVariantClear(&value);
    }
    return info;
}

} // namespace boson::shell
