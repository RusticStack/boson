#include <boson/shell/toast.hpp>

#include <boson/core/result.hpp>
#include <boson/shell/app_id.hpp>

#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <windows.foundation.collections.h>
#include <windows.foundation.h>
#include <windows.ui.notifications.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <utility>

namespace boson::shell {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Implements;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;

namespace wun = ABI::Windows::UI::Notifications;
namespace wdx = ABI::Windows::Data::Xml::Dom;
namespace wf = ABI::Windows::Foundation;
namespace wfc = ABI::Windows::Foundation::Collections;

using ActivatedHandler = wf::ITypedEventHandler<wun::ToastNotification*, IInspectable*>;
using DismissedHandler = wf::ITypedEventHandler<wun::ToastNotification*, wun::ToastDismissedEventArgs*>;
using FailedHandler = wf::ITypedEventHandler<wun::ToastNotification*, wun::ToastFailedEventArgs*>;

const char* activation_name(ToastActivationType type) noexcept {
    switch (type) {
    case ToastActivationType::Foreground: return "foreground";
    case ToastActivationType::Background: return "background";
    case ToastActivationType::Protocol: return "protocol";
    }
    return "foreground";
}

const char* scenario_name(ToastScenario scenario) noexcept {
    switch (scenario) {
    case ToastScenario::Default: return nullptr;
    case ToastScenario::Reminder: return "reminder";
    case ToastScenario::Alarm: return "alarm";
    case ToastScenario::IncomingCall: return "incomingCall";
    case ToastScenario::Urgent: return "urgent";
    }
    return nullptr;
}

void append_attr(std::string& out, std::string_view name, std::string_view value) {
    out += ' ';
    out += name;
    out += "=\"";
    out += xml_escape(value);
    out += '"';
}

std::string hstring_to_utf8(HSTRING value) {
    UINT32 length = 0;
    const wchar_t* raw = ::WindowsGetStringRawBuffer(value, &length);
    if (raw == nullptr || length == 0) {
        return {};
    }
    return core::wide_to_utf8(std::wstring_view(raw, length));
}

bool make_hstring(std::string_view utf8, HString& out) {
    const std::wstring wide = core::utf8_to_wide(utf8);
    return SUCCEEDED(out.Set(wide.c_str(), static_cast<unsigned int>(wide.size())));
}

std::string format_progress_value(const std::optional<double>& value) {
    if (!value.has_value()) {
        return "indeterminate";
    }
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.4f", std::clamp(*value, 0.0, 1.0));
    return buffer;
}

/// NotificationData holding the four progress bindings.
ComPtr<wun::INotificationData> make_progress_data(const ToastProgress& progress) {
    ComPtr<IInspectable> inspectable;
    if (!BOSON_HR_CHECK(::RoActivateInstance(
            HStringReference(RuntimeClass_Windows_UI_Notifications_NotificationData).Get(), &inspectable))) {
        return nullptr;
    }
    ComPtr<wun::INotificationData> data;
    if (!BOSON_HR_CHECK(inspectable.As(&data))) {
        return nullptr;
    }
    ComPtr<wfc::IMap<HSTRING, HSTRING>> values;
    if (!BOSON_HR_CHECK(data->get_Values(&values))) {
        return nullptr;
    }
    const std::pair<const wchar_t*, std::string> entries[] = {
        {L"progressTitle", progress.title},
        {L"progressValue", format_progress_value(progress.value)},
        {L"progressValueString", progress.value_label},
        {L"progressStatus", progress.status},
    };
    for (const auto& [key, value] : entries) {
        HString hvalue;
        if (!make_hstring(value, hvalue)) {
            return nullptr;
        }
        boolean replaced = false;
        if (!BOSON_HR_CHECK(values->Insert(HStringReference(key).Get(), hvalue.Get(), &replaced))) {
            return nullptr;
        }
    }
    data->put_SequenceNumber(0); // 0 = always apply
    return data;
}

ComPtr<wun::IToastNotificationManagerStatics> manager_statics() {
    ComPtr<wun::IToastNotificationManagerStatics> statics;
    if (!BOSON_HR_CHECK(wf::GetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(), &statics))) {
        return nullptr;
    }
    return statics;
}

ComPtr<wun::IToastNotifier> create_notifier(const std::wstring& app_id) {
    const auto statics = manager_statics();
    if (statics == nullptr || app_id.empty()) {
        return nullptr;
    }
    ComPtr<wun::IToastNotifier> notifier;
    if (!BOSON_HR_CHECK(statics->CreateToastNotifierWithId(
            HStringReference(app_id.c_str(), static_cast<unsigned int>(app_id.size())).Get(), &notifier))) {
        return nullptr;
    }
    return notifier;
}

ComPtr<wun::IToastNotificationHistory> history() {
    const auto statics = manager_statics();
    if (statics == nullptr) {
        return nullptr;
    }
    ComPtr<wun::IToastNotificationManagerStatics2> statics2;
    if (!BOSON_HR_CHECK(statics.As(&statics2))) {
        return nullptr;
    }
    ComPtr<wun::IToastNotificationHistory> out;
    if (!BOSON_HR_CHECK(statics2->get_History(&out))) {
        return nullptr;
    }
    return out;
}

std::string read_user_input(IInspectable* args, std::string_view id) {
    ComPtr<IInspectable> inspectable(args);
    ComPtr<wun::IToastActivatedEventArgs2> args2;
    if (FAILED(inspectable.As(&args2))) {
        return {};
    }
    ComPtr<wfc::IPropertySet> set;
    if (FAILED(args2->get_UserInput(&set)) || set == nullptr) {
        return {};
    }
    ComPtr<wfc::IMap<HSTRING, IInspectable*>> map;
    if (FAILED(set.As(&map))) {
        return {};
    }
    HString key;
    if (!make_hstring(id, key)) {
        return {};
    }
    ComPtr<IInspectable> value;
    if (FAILED(map->Lookup(key.Get(), &value)) || value == nullptr) {
        return {};
    }
    ComPtr<wf::IPropertyValue> property;
    if (FAILED(value.As(&property))) {
        return {};
    }
    HString text;
    if (FAILED(property->GetString(text.GetAddressOf()))) {
        return {};
    }
    return hstring_to_utf8(text.Get());
}

} // namespace

// ---- XML ---------------------------------------------------------------------------------------------

std::string xml_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:
            // Control characters other than tab/newline are invalid in XML 1.0.
            if (static_cast<unsigned char>(ch) < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') {
                continue;
            }
            out += ch;
            break;
        }
    }
    return out;
}

std::string to_image_uri(std::string_view path_or_uri) {
    if (path_or_uri.empty()) {
        return {};
    }
    if (path_or_uri.find("://") != std::string_view::npos || path_or_uri.starts_with("ms-app")) {
        return std::string(path_or_uri);
    }
    const bool drive = path_or_uri.size() >= 3 && std::isalpha(static_cast<unsigned char>(path_or_uri[0])) &&
                       path_or_uri[1] == ':' && (path_or_uri[2] == '\\' || path_or_uri[2] == '/');
    const bool unc = path_or_uri.starts_with("\\\\");
    if (!drive && !unc) {
        return std::string(path_or_uri);
    }
    std::string out = unc ? "file:" : "file:///";
    for (const char ch : path_or_uri) {
        switch (ch) {
        case '\\': out += '/'; break;
        case ' ': out += "%20"; break;
        case '#': out += "%23"; break;
        case '%': out += "%25"; break;
        default: out += ch; break;
        }
    }
    return out;
}

std::string build_toast_xml(const ToastContent& content) {
    std::string xml = "<toast";
    if (!content.launch_arguments.empty()) {
        append_attr(xml, "launch", content.launch_arguments);
    }
    append_attr(xml, "activationType", activation_name(content.activation));
    if (const char* scenario = scenario_name(content.scenario)) {
        append_attr(xml, "scenario", scenario);
    }
    if (content.duration != ToastDuration::Default) {
        append_attr(xml, "duration", content.duration == ToastDuration::Long ? "long" : "short");
    }
    xml += "><visual><binding template=\"ToastGeneric\">";
    if (!content.title.empty()) {
        xml += "<text>" + xml_escape(content.title) + "</text>";
    }
    if (!content.body.empty()) {
        xml += "<text>" + xml_escape(content.body) + "</text>";
    }
    if (!content.attribution.empty()) {
        xml += "<text placement=\"attribution\">" + xml_escape(content.attribution) + "</text>";
    }
    if (!content.app_logo.empty()) {
        xml += "<image placement=\"appLogoOverride\"";
        append_attr(xml, "src", to_image_uri(content.app_logo));
        if (content.app_logo_circle) {
            append_attr(xml, "hint-crop", "circle");
        }
        xml += "/>";
    }
    if (!content.hero_image.empty()) {
        xml += "<image placement=\"hero\"";
        append_attr(xml, "src", to_image_uri(content.hero_image));
        xml += "/>";
    }
    if (!content.inline_image.empty()) {
        xml += "<image";
        append_attr(xml, "src", to_image_uri(content.inline_image));
        xml += "/>";
    }
    if (content.progress.has_value()) {
        xml += "<progress title=\"{progressTitle}\" value=\"{progressValue}\" "
               "valueStringOverride=\"{progressValueString}\" status=\"{progressStatus}\"/>";
    }
    xml += "</binding></visual>";

    if (!content.inputs.empty() || !content.buttons.empty()) {
        xml += "<actions>";
        for (const auto& input : content.inputs) {
            xml += "<input";
            append_attr(xml, "id", input.id);
            append_attr(xml, "type", input.options.empty() ? "text" : "selection");
            if (!input.title.empty()) {
                append_attr(xml, "title", input.title);
            }
            if (!input.placeholder.empty() && input.options.empty()) {
                append_attr(xml, "placeHolderContent", input.placeholder);
            }
            if (!input.default_value.empty()) {
                append_attr(xml, "defaultInput", input.default_value);
            }
            if (input.options.empty()) {
                xml += "/>";
                continue;
            }
            xml += ">";
            for (const auto& option : input.options) {
                xml += "<selection";
                append_attr(xml, "id", option.id);
                append_attr(xml, "content", option.content);
                xml += "/>";
            }
            xml += "</input>";
        }
        for (const auto& button : content.buttons) {
            xml += "<action";
            append_attr(xml, "content", button.content);
            append_attr(xml, "arguments", button.arguments);
            append_attr(xml, "activationType", activation_name(button.activation));
            if (!button.input_id.empty()) {
                append_attr(xml, "hint-inputId", button.input_id);
            }
            if (!button.image.empty()) {
                append_attr(xml, "imageUri", to_image_uri(button.image));
            }
            if (button.context_menu) {
                append_attr(xml, "placement", "contextMenu");
            }
            xml += "/>";
        }
        xml += "</actions>";
    }

    if (content.silent) {
        xml += "<audio silent=\"true\"/>";
    } else if (!content.sound.empty() || content.loop_sound) {
        xml += "<audio";
        if (!content.sound.empty()) {
            append_attr(xml, "src", content.sound);
        }
        if (content.loop_sound) {
            append_attr(xml, "loop", "true");
        }
        xml += "/>";
    }
    xml += "</toast>";
    return xml;
}

// ---- ToastNotifier -----------------------------------------------------------------------------------

struct ToastNotifier::State {
    struct Active {
        std::string tag;
        std::string group;
        std::vector<std::string> input_ids;
        ComPtr<wun::IToastNotification> toast;
    };

    std::wstring app_id;
    mutable std::mutex mutex;
    EventFn on_event;
    std::vector<Active> active;
    std::atomic<std::uint64_t> next_tag{1};

    /// Removes the entry for toast and returns its tag/group/input ids.
    std::optional<Active> take(wun::IToastNotification* toast) {
        std::scoped_lock lock(mutex);
        for (auto it = active.begin(); it != active.end(); ++it) {
            if (it->toast.Get() == toast) {
                Active out = std::move(*it);
                active.erase(it);
                return out;
            }
        }
        return std::nullopt;
    }

    void emit(const ToastEvent& event) {
        EventFn callback;
        {
            std::scoped_lock lock(mutex);
            callback = on_event;
        }
        if (callback) {
            try {
                callback(event);
            } catch (...) {
                // Never let a C++ exception unwind into the WinRT event source.
            }
        }
    }
};

ToastNotifier::ToastNotifier(std::wstring app_id) : m_state(std::make_shared<State>()) {
    // Keeps the process MTA alive, so WinRT objects and callbacks stay valid on every thread.
    if (FAILED(::CoIncrementMTAUsage(&m_mta_cookie))) {
        m_mta_cookie = nullptr;
    }
    if (app_id.empty()) {
        app_id = shell::app_user_model_id().value_or(std::wstring{});
    }
    m_state->app_id = std::move(app_id);
}

ToastNotifier::~ToastNotifier() {
    {
        std::scoped_lock lock(m_state->mutex);
        m_state->on_event = nullptr;
        m_state->active.clear();
    }
    if (m_mta_cookie != nullptr) {
        ::CoDecrementMTAUsage(m_mta_cookie);
    }
}

bool ToastNotifier::is_supported() noexcept {
    CO_MTA_USAGE_COOKIE cookie = nullptr;
    const bool mta = SUCCEEDED(::CoIncrementMTAUsage(&cookie));
    bool ok = false;
    {
        ComPtr<wun::IToastNotificationManagerStatics> statics;
        ok = SUCCEEDED(wf::GetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(), &statics));
    }
    if (mta) {
        ::CoDecrementMTAUsage(cookie);
    }
    return ok;
}

const std::wstring& ToastNotifier::app_user_model_id() const noexcept {
    return m_state->app_id;
}

ToastSetting ToastNotifier::setting() const {
    const auto notifier = create_notifier(m_state->app_id);
    if (notifier == nullptr) {
        return ToastSetting::Unknown;
    }
    wun::NotificationSetting value = wun::NotificationSetting_Enabled;
    // HRESULT_FROM_WIN32(ERROR_NOT_FOUND) is normal for unpackaged apps before their first toast.
    if (FAILED(notifier->get_Setting(&value))) {
        return ToastSetting::Unknown;
    }
    switch (value) {
    case wun::NotificationSetting_Enabled: return ToastSetting::Enabled;
    case wun::NotificationSetting_DisabledForApplication: return ToastSetting::DisabledForApplication;
    case wun::NotificationSetting_DisabledForUser: return ToastSetting::DisabledForUser;
    case wun::NotificationSetting_DisabledByGroupPolicy: return ToastSetting::DisabledByGroupPolicy;
    case wun::NotificationSetting_DisabledByManifest: return ToastSetting::DisabledByManifest;
    default: return ToastSetting::Unknown;
    }
}

void ToastNotifier::set_event_callback(EventFn callback) {
    std::scoped_lock lock(m_state->mutex);
    m_state->on_event = std::move(callback);
}

std::size_t ToastNotifier::active_count() const {
    std::scoped_lock lock(m_state->mutex);
    return m_state->active.size();
}

ToastShowResult ToastNotifier::show(const ToastContent& content) {
    ToastShowResult result;
    result.tag = content.tag.empty() ? "boson-" + std::to_string(m_state->next_tag.fetch_add(1)) : content.tag;
    result.group = content.group.empty() ? std::string(kDefaultToastGroup) : content.group;
    if (content.buttons.size() > kMaxToastButtons || content.inputs.size() > kMaxToastInputs) {
        result.hr = E_INVALIDARG;
        return result;
    }
    if (m_state->app_id.empty()) {
        result.hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); // no AppUserModelID
        return result;
    }

    const auto notifier = create_notifier(m_state->app_id);
    if (notifier == nullptr) {
        result.hr = E_FAIL;
        return result;
    }

    // XML document
    ComPtr<IInspectable> doc_inspectable;
    result.hr = ::RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), &doc_inspectable);
    if (!core::hr_check(result.hr, "RoActivateInstance(XmlDocument)")) {
        return result;
    }
    ComPtr<wdx::IXmlDocumentIO> doc_io;
    ComPtr<wdx::IXmlDocument> doc;
    if (FAILED(result.hr = doc_inspectable.As(&doc_io)) || FAILED(result.hr = doc_inspectable.As(&doc))) {
        return result;
    }
    const std::wstring xml = core::utf8_to_wide(build_toast_xml(content));
    result.hr = doc_io->LoadXml(HStringReference(xml.c_str(), static_cast<unsigned int>(xml.size())).Get());
    if (!core::hr_check(result.hr, "IXmlDocumentIO::LoadXml(toast)")) {
        return result;
    }

    // Notification
    ComPtr<wun::IToastNotificationFactory> factory;
    result.hr = wf::GetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(), &factory);
    if (!core::hr_check(result.hr, "GetActivationFactory(ToastNotification)")) {
        return result;
    }
    ComPtr<wun::IToastNotification> toast;
    result.hr = factory->CreateToastNotification(doc.Get(), &toast);
    if (!core::hr_check(result.hr, "CreateToastNotification")) {
        return result;
    }

    ComPtr<wun::IToastNotification2> toast2;
    if (SUCCEEDED(toast.As(&toast2))) {
        HString tag;
        HString group;
        make_hstring(result.tag, tag);
        make_hstring(result.group, group);
        BOSON_HR_CHECK(toast2->put_Tag(tag.Get()));
        BOSON_HR_CHECK(toast2->put_Group(group.Get()));
        if (content.suppress_popup) {
            BOSON_HR_CHECK(toast2->put_SuppressPopup(true));
        }
    }
    if (content.progress.has_value()) {
        ComPtr<wun::IToastNotification4> toast4;
        const auto data = make_progress_data(*content.progress);
        if (data != nullptr && SUCCEEDED(toast.As(&toast4))) {
            BOSON_HR_CHECK(toast4->put_Data(data.Get()));
        }
    }
    if (content.expires_after.has_value() && content.expires_after->count() > 0) {
        ComPtr<wf::IPropertyValueStatics> values;
        if (SUCCEEDED(wf::GetActivationFactory(HStringReference(RuntimeClass_Windows_Foundation_PropertyValue).Get(), &values))) {
            FILETIME now{};
            ::GetSystemTimeAsFileTime(&now);
            const auto ticks = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            wf::DateTime when{};
            when.UniversalTime = static_cast<INT64>(ticks + static_cast<std::uint64_t>(content.expires_after->count()) * 10'000'000ull);
            ComPtr<IInspectable> boxed;
            ComPtr<wf::IReference<wf::DateTime>> reference;
            if (SUCCEEDED(values->CreateDateTime(when, &boxed)) && SUCCEEDED(boxed.As(&reference))) {
                BOSON_HR_CHECK(toast->put_ExpirationTime(reference.Get()));
            }
        }
    }

    // Events
    std::vector<std::string> input_ids;
    input_ids.reserve(content.inputs.size());
    for (const auto& input : content.inputs) {
        input_ids.push_back(input.id);
    }
    const std::weak_ptr<State> weak = m_state;
    EventRegistrationToken token{};
    BOSON_HR_CHECK(toast->add_Activated(
        Callback<Implements<RuntimeClassFlags<ClassicCom>, ActivatedHandler, FtmBase>>(
            [weak](wun::IToastNotification* sender, IInspectable* args) -> HRESULT {
                const auto state = weak.lock();
                if (state == nullptr) {
                    return S_OK;
                }
                const auto entry = state->take(sender);
                ToastEvent event;
                event.kind = ToastEventKind::Activated;
                if (entry.has_value()) {
                    event.tag = entry->tag;
                    event.group = entry->group;
                }
                ComPtr<IInspectable> inspectable(args);
                ComPtr<wun::IToastActivatedEventArgs> activated;
                if (args != nullptr && SUCCEEDED(inspectable.As(&activated))) {
                    HString arguments;
                    if (SUCCEEDED(activated->get_Arguments(arguments.GetAddressOf()))) {
                        event.arguments = hstring_to_utf8(arguments.Get());
                    }
                    if (entry.has_value()) {
                        for (const auto& id : entry->input_ids) {
                            event.user_input[id] = read_user_input(args, id);
                        }
                    }
                }
                state->emit(event);
                return S_OK;
            }).Get(),
        &token));
    BOSON_HR_CHECK(toast->add_Dismissed(
        Callback<Implements<RuntimeClassFlags<ClassicCom>, DismissedHandler, FtmBase>>(
            [weak](wun::IToastNotification* sender, wun::IToastDismissedEventArgs* args) -> HRESULT {
                const auto state = weak.lock();
                if (state == nullptr) {
                    return S_OK;
                }
                const auto entry = state->take(sender);
                ToastEvent event;
                event.kind = ToastEventKind::Dismissed;
                if (entry.has_value()) {
                    event.tag = entry->tag;
                    event.group = entry->group;
                }
                wun::ToastDismissalReason reason = wun::ToastDismissalReason_UserCanceled;
                if (args != nullptr && SUCCEEDED(args->get_Reason(&reason))) {
                    event.reason = reason == wun::ToastDismissalReason_ApplicationHidden ? ToastDismissReason::ApplicationHidden
                                   : reason == wun::ToastDismissalReason_TimedOut        ? ToastDismissReason::TimedOut
                                                                                         : ToastDismissReason::UserCanceled;
                }
                state->emit(event);
                return S_OK;
            }).Get(),
        &token));
    BOSON_HR_CHECK(toast->add_Failed(
        Callback<Implements<RuntimeClassFlags<ClassicCom>, FailedHandler, FtmBase>>(
            [weak](wun::IToastNotification* sender, wun::IToastFailedEventArgs* args) -> HRESULT {
                const auto state = weak.lock();
                if (state == nullptr) {
                    return S_OK;
                }
                const auto entry = state->take(sender);
                ToastEvent event;
                event.kind = ToastEventKind::Failed;
                if (entry.has_value()) {
                    event.tag = entry->tag;
                    event.group = entry->group;
                }
                if (args != nullptr) {
                    args->get_ErrorCode(&event.error);
                }
                state->emit(event);
                return S_OK;
            }).Get(),
        &token));

    {
        std::scoped_lock lock(m_state->mutex);
        // A toast replaced under the same tag/group never reports again; forget the old entry.
        std::erase_if(m_state->active, [&](const State::Active& a) { return a.tag == result.tag && a.group == result.group; });
        m_state->active.push_back(State::Active{result.tag, result.group, std::move(input_ids), toast});
    }
    result.hr = notifier->Show(toast.Get());
    if (!core::hr_check(result.hr, "IToastNotifier::Show")) {
        m_state->take(toast.Get());
        return result;
    }
    result.ok = true;
    return result;
}

bool ToastNotifier::update_progress(std::string_view tag, std::string_view group, const ToastProgress& progress) {
    const auto notifier = create_notifier(m_state->app_id);
    if (notifier == nullptr || tag.empty()) {
        return false;
    }
    ComPtr<wun::IToastNotifier2> notifier2;
    if (!BOSON_HR_CHECK(notifier.As(&notifier2))) {
        return false;
    }
    const auto data = make_progress_data(progress);
    if (data == nullptr) {
        return false;
    }
    HString htag;
    HString hgroup;
    if (!make_hstring(tag, htag) || !make_hstring(group.empty() ? kDefaultToastGroup : group, hgroup)) {
        return false;
    }
    wun::NotificationUpdateResult update = wun::NotificationUpdateResult_Failed;
    if (!BOSON_HR_CHECK(notifier2->UpdateWithTagAndGroup(data.Get(), htag.Get(), hgroup.Get(), &update))) {
        return false;
    }
    return update == wun::NotificationUpdateResult_Succeeded;
}

bool ToastNotifier::hide(std::string_view tag, std::string_view group) {
    const std::string_view effective_group = group.empty() ? kDefaultToastGroup : group;
    ComPtr<wun::IToastNotification> toast;
    {
        std::scoped_lock lock(m_state->mutex);
        for (const auto& entry : m_state->active) {
            if (entry.tag == tag && entry.group == effective_group) {
                toast = entry.toast;
                break;
            }
        }
    }
    if (toast == nullptr) {
        return false;
    }
    const auto notifier = create_notifier(m_state->app_id);
    return notifier != nullptr && BOSON_HR_CHECK(notifier->Hide(toast.Get()));
}

bool ToastNotifier::remove(std::string_view tag, std::string_view group) {
    const auto hist = history();
    if (hist == nullptr || tag.empty() || m_state->app_id.empty()) {
        return false;
    }
    HString htag;
    HString hgroup;
    if (!make_hstring(tag, htag) || !make_hstring(group.empty() ? kDefaultToastGroup : group, hgroup)) {
        return false;
    }
    const std::wstring& id = m_state->app_id;
    return BOSON_HR_CHECK(hist->RemoveGroupedTagWithId(
        htag.Get(), hgroup.Get(), HStringReference(id.c_str(), static_cast<unsigned int>(id.size())).Get()));
}

bool ToastNotifier::clear_all() {
    const auto hist = history();
    if (hist == nullptr || m_state->app_id.empty()) {
        return false;
    }
    const std::wstring& id = m_state->app_id;
    return BOSON_HR_CHECK(hist->ClearWithId(HStringReference(id.c_str(), static_cast<unsigned int>(id.size())).Get()));
}

} // namespace boson::shell
