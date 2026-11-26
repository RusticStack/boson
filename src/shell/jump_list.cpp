#include <boson/shell/jump_list.hpp>

#include <boson/app/lifecycle.hpp>
#include <boson/core/result.hpp>
#include <boson/shell/app_id.hpp>

#include "shell_util.hpp"

#include <objectarray.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <optional>

namespace boson::shell {

namespace {

using Microsoft::WRL::ComPtr;

std::optional<std::wstring> resolve_app_id(std::wstring_view app_id) {
    if (!app_id.empty()) {
        if (!is_valid_app_user_model_id(app_id)) {
            return std::nullopt;
        }
        return std::wstring(app_id);
    }
    return app_user_model_id();
}

ComPtr<IShellLinkW> make_link(const JumpListItem& item) {
    ComPtr<IShellLinkW> link;
    if (!BOSON_HR_CHECK(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        return nullptr;
    }
    ComPtr<IPropertyStore> store;
    if (!BOSON_HR_CHECK(link.As(&store))) {
        return nullptr;
    }
    if (item.separator) {
        PROPVARIANT value{};
        ::PropVariantInit(&value);
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_TRUE;
        if (!BOSON_HR_CHECK(store->SetValue(detail::kPkeyIsDestListSeparator, value)) || !BOSON_HR_CHECK(store->Commit())) {
            return nullptr;
        }
        return link;
    }
    const std::wstring program = item.program.empty() ? app::executable_path() : item.program;
    if (!BOSON_HR_CHECK(link->SetPath(program.c_str()))) {
        return nullptr;
    }
    if (!item.arguments.empty()) {
        BOSON_HR_CHECK(link->SetArguments(item.arguments.c_str()));
    }
    if (!item.working_directory.empty()) {
        BOSON_HR_CHECK(link->SetWorkingDirectory(item.working_directory.c_str()));
    }
    if (!item.description.empty()) {
        BOSON_HR_CHECK(link->SetDescription(item.description.c_str()));
    }
    const std::wstring& icon = item.icon_path.empty() ? program : item.icon_path;
    BOSON_HR_CHECK(link->SetIconLocation(icon.c_str(), item.icon_index));
    const detail::PropString title(item.title.empty() ? program : item.title);
    if (!title.ok() || !BOSON_HR_CHECK(store->SetValue(detail::kPkeyTitle, title.get())) || !BOSON_HR_CHECK(store->Commit())) {
        return nullptr;
    }
    return link;
}

struct LinkKey {
    std::wstring path;
    std::wstring arguments;
};

std::vector<LinkKey> removed_links(IObjectArray* removed) {
    std::vector<LinkKey> out;
    if (removed == nullptr) {
        return out;
    }
    UINT count = 0;
    if (FAILED(removed->GetCount(&count))) {
        return out;
    }
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IShellLinkW> link;
        if (FAILED(removed->GetAt(i, IID_PPV_ARGS(&link)))) {
            continue;
        }
        wchar_t path[MAX_PATH]{};
        wchar_t args[INFOTIPSIZE]{};
        link->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH);
        link->GetArguments(args, INFOTIPSIZE);
        out.push_back(LinkKey{detail::lower_ascii(path), args});
    }
    return out;
}

bool is_removed(const std::vector<LinkKey>& removed, const JumpListItem& item) {
    if (item.separator || removed.empty()) {
        return false;
    }
    const std::wstring path = detail::lower_ascii(item.program.empty() ? app::executable_path() : item.program);
    for (const auto& key : removed) {
        if (key.path == path && key.arguments == item.arguments) {
            return true;
        }
    }
    return false;
}

ComPtr<IObjectCollection> make_collection() {
    ComPtr<IObjectCollection> collection;
    BOSON_HR_CHECK(::CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&collection)));
    return collection;
}

} // namespace

JumpListResult set_jump_list(const JumpListSpec& spec, std::wstring_view app_id) {
    JumpListResult result;
    const detail::ScopedCom com;
    if (!com.ok()) {
        result.hr = CO_E_NOTINITIALIZED;
        return result;
    }
    ComPtr<ICustomDestinationList> list;
    result.hr = ::CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list));
    if (!core::hr_check(result.hr, "CoCreateInstance(CLSID_DestinationList)")) {
        return result;
    }
    const auto id = resolve_app_id(app_id);
    if (!app_id.empty() && !id.has_value()) {
        result.hr = E_INVALIDARG;
        return result;
    }
    if (id.has_value()) {
        result.hr = list->SetAppID(id->c_str());
        if (!core::hr_check(result.hr, "ICustomDestinationList::SetAppID")) {
            return result;
        }
    }

    UINT max_slots = 0;
    ComPtr<IObjectArray> removed;
    result.hr = list->BeginList(&max_slots, IID_PPV_ARGS(&removed));
    if (!core::hr_check(result.hr, "ICustomDestinationList::BeginList")) {
        return result;
    }
    result.max_slots = max_slots;
    const std::vector<LinkKey> removed_keys = removed_links(removed.Get());

    for (const auto& category : spec.categories) {
        ComPtr<IObjectCollection> collection = make_collection();
        if (collection == nullptr || category.name.empty()) {
            result.rejected_categories.push_back(category.name);
            continue;
        }
        UINT added = 0;
        for (const auto& item : category.items) {
            if (item.separator) {
                continue; // separators are only valid among tasks
            }
            if (is_removed(removed_keys, item)) {
                ++result.skipped_removed;
                continue;
            }
            if (const auto link = make_link(item); link != nullptr && SUCCEEDED(collection->AddObject(link.Get()))) {
                ++added;
            }
        }
        if (added == 0) {
            continue;
        }
        ComPtr<IObjectArray> array;
        if (FAILED(collection.As(&array)) || FAILED(list->AppendCategory(category.name.c_str(), array.Get()))) {
            // E_ACCESSDENIED when the user turned off "Show recently opened items".
            result.rejected_categories.push_back(category.name);
        }
    }

    if (spec.show_frequent && FAILED(list->AppendKnownCategory(KDC_FREQUENT))) {
        result.rejected_categories.emplace_back(L"#frequent");
    }
    if (spec.show_recent && FAILED(list->AppendKnownCategory(KDC_RECENT))) {
        result.rejected_categories.emplace_back(L"#recent");
    }

    if (!spec.tasks.empty()) {
        ComPtr<IObjectCollection> tasks = make_collection();
        if (tasks != nullptr) {
            UINT added = 0;
            for (const auto& item : spec.tasks) {
                if (const auto link = make_link(item); link != nullptr && SUCCEEDED(tasks->AddObject(link.Get()))) {
                    ++added;
                }
            }
            ComPtr<IObjectArray> array;
            if (added > 0 && SUCCEEDED(tasks.As(&array))) {
                result.hr = list->AddUserTasks(array.Get());
                if (!core::hr_check(result.hr, "ICustomDestinationList::AddUserTasks")) {
                    list->AbortList();
                    return result;
                }
            }
        }
    }

    result.hr = list->CommitList();
    result.ok = core::hr_check(result.hr, "ICustomDestinationList::CommitList");
    return result;
}

bool clear_jump_list(std::wstring_view app_id) {
    const detail::ScopedCom com;
    if (!com.ok()) {
        return false;
    }
    ComPtr<ICustomDestinationList> list;
    if (!BOSON_HR_CHECK(::CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list)))) {
        return false;
    }
    const auto id = resolve_app_id(app_id);
    if (!app_id.empty() && !id.has_value()) {
        return false;
    }
    return core::hr_check(list->DeleteList(id.has_value() ? id->c_str() : nullptr), "ICustomDestinationList::DeleteList");
}

bool add_recent_document(std::wstring_view path, std::wstring_view app_id) {
    if (path.empty()) {
        return false;
    }
    const detail::ScopedCom com;
    if (!com.ok()) {
        return false;
    }
    const auto id = resolve_app_id(app_id);
    const std::wstring file(path);
    if (!id.has_value()) {
        if (!app_id.empty()) {
            return false;
        }
        ::SHAddToRecentDocs(SHARD_PATHW, file.c_str());
        return true;
    }
    ComPtr<IShellItem> item;
    if (!BOSON_HR_CHECK(::SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return false;
    }
    SHARDAPPIDINFO info{};
    info.psi = item.Get();
    info.pszAppID = id->c_str();
    ::SHAddToRecentDocs(SHARD_APPIDINFO, &info);
    return true;
}

bool clear_recent_documents(std::wstring_view app_id) {
    const detail::ScopedCom com;
    if (!com.ok()) {
        return false;
    }
    ComPtr<IApplicationDestinations> destinations;
    if (!BOSON_HR_CHECK(::CoCreateInstance(CLSID_ApplicationDestinations, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&destinations)))) {
        return false;
    }
    const auto id = resolve_app_id(app_id);
    if (!app_id.empty() && !id.has_value()) {
        return false;
    }
    if (id.has_value() && !BOSON_HR_CHECK(destinations->SetAppID(id->c_str()))) {
        return false;
    }
    const HRESULT hr = destinations->RemoveAllDestinations();
    // Nothing recorded yet is not an error.
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || core::hr_check(hr, "RemoveAllDestinations");
}

} // namespace boson::shell
