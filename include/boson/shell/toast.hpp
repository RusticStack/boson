#pragma once

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::shell {

enum class ToastScenario { Default, Reminder, Alarm, IncomingCall, Urgent };
enum class ToastDuration { Default, Short, Long };
enum class ToastActivationType { Foreground, Background, Protocol };

struct ToastButton {
    std::string content;
    std::string arguments; ///< delivered on activation (a URI for Protocol)
    ToastActivationType activation = ToastActivationType::Foreground;
    std::string input_id;  ///< places the button next to this text input (quick reply)
    std::string image;     ///< path or URI of a 16x16 button icon
    bool context_menu = false; ///< add to the toast's right-click menu instead of a button
};

struct ToastSelectionOption {
    std::string id;
    std::string content;
};

/// @brief Text box (options empty) or drop-down (options non-empty). Values arrive as user_input[id].
struct ToastInput {
    std::string id;
    std::string title;
    std::string placeholder;
    std::string default_value;
    std::vector<ToastSelectionOption> options;
};

/// @brief Data-bound progress bar; update it later with ToastNotifier::update_progress.
struct ToastProgress {
    std::string title;
    std::optional<double> value; ///< 0..1; std::nullopt = indeterminate
    std::string value_label;     ///< replaces the percentage text when set
    std::string status;          ///< e.g. "Downloading..."
};

/// @brief ToastGeneric content. Image fields accept absolute file paths (converted to file:/// URIs) or
/// URIs; unpackaged apps can only show local images.
struct ToastContent {
    std::string title;
    std::string body;
    std::string attribution;
    std::string app_logo;
    bool app_logo_circle = false;
    std::string hero_image;
    std::string inline_image;
    std::string launch_arguments; ///< delivered when the toast body is clicked
    ToastActivationType activation = ToastActivationType::Foreground;
    std::vector<ToastButton> buttons; ///< at most 5 (including context-menu entries)
    std::vector<ToastInput> inputs;   ///< at most 5
    std::optional<ToastProgress> progress;
    ToastScenario scenario = ToastScenario::Default;
    ToastDuration duration = ToastDuration::Default;
    bool silent = false;
    std::string sound; ///< e.g. "ms-winsoundevent:Notification.Reminder"; empty = default sound
    bool loop_sound = false;
    std::string tag;   ///< empty = generated; identifies the toast for update/hide/remove
    std::string group; ///< empty = "boson"
    bool suppress_popup = false; ///< straight to Notification Center
    std::optional<std::chrono::seconds> expires_after; ///< removed from Notification Center afterwards
};

inline constexpr std::string_view kDefaultToastGroup = "boson";
inline constexpr std::size_t kMaxToastButtons = 5;
inline constexpr std::size_t kMaxToastInputs = 5;

/// @brief XML-escapes text for attribute and element content.
[[nodiscard]] std::string xml_escape(std::string_view text);
/// @brief Absolute Windows path -> file:/// URI; URIs (scheme://, ms-app*:) are returned unchanged.
[[nodiscard]] std::string to_image_uri(std::string_view path_or_uri);
/// @brief The toast XML (UTF-8) for content. The progress bar is data-bound ({progressValue} ...).
[[nodiscard]] std::string build_toast_xml(const ToastContent& content);

enum class ToastDismissReason { UserCanceled, ApplicationHidden, TimedOut };

enum class ToastSetting {
    Enabled,
    DisabledForApplication,
    DisabledForUser,
    DisabledByGroupPolicy,
    DisabledByManifest,
    Unknown,
};

enum class ToastEventKind { Activated, Dismissed, Failed };

struct ToastEvent {
    ToastEventKind kind = ToastEventKind::Activated;
    std::string tag;
    std::string group;
    std::string arguments;                        ///< Activated
    std::map<std::string, std::string> user_input; ///< Activated: input id -> text / selection id
    ToastDismissReason reason = ToastDismissReason::UserCanceled; ///< Dismissed
    HRESULT error = S_OK;                          ///< Failed
};

struct ToastShowResult {
    bool ok = false;
    HRESULT hr = S_OK;
    std::string tag;
    std::string group;
};

/// @brief Windows.UI.Notifications toasts through the raw WinRT ABI (no Windows App SDK).
///
/// Unpackaged apps need their AppUserModelID registered with the shell, either with
/// register_app_user_model_id() or a Start-menu shortcut (create_start_menu_shortcut()); see
/// toast_activation.hpp. All members are thread-safe; event callbacks run on a WinRT thread-pool thread.
class ToastNotifier {
public:
    using EventFn = std::function<void(const ToastEvent& event)>;

    /// @brief app_id empty = the process explicit AppUserModelID at construction time.
    explicit ToastNotifier(std::wstring app_id = {});
    ~ToastNotifier();

    ToastNotifier(const ToastNotifier&) = delete;
    ToastNotifier& operator=(const ToastNotifier&) = delete;
    ToastNotifier(ToastNotifier&&) = delete;
    ToastNotifier& operator=(ToastNotifier&&) = delete;

    /// @brief true when the WinRT toast APIs exist (Windows 10+).
    [[nodiscard]] static bool is_supported() noexcept;
    [[nodiscard]] const std::wstring& app_user_model_id() const noexcept;
    [[nodiscard]] ToastSetting setting() const;

    [[nodiscard]] ToastShowResult show(const ToastContent& content);
    /// @brief Updates a shown progress bar (IToastNotifier2::UpdateWithTagAndGroup). false when the toast
    /// is gone from both the screen and Notification Center.
    bool update_progress(std::string_view tag, std::string_view group, const ToastProgress& progress);
    /// @brief Hides a toast this notifier showed (it stays in Notification Center).
    bool hide(std::string_view tag, std::string_view group = {});
    /// @brief Removes a toast from Notification Center (and the screen).
    bool remove(std::string_view tag, std::string_view group = {});
    /// @brief Removes every toast of this app from Notification Center.
    bool clear_all();

    void set_event_callback(EventFn callback);
    /// @brief Toasts shown by this notifier that have not yet been activated, dismissed or failed.
    [[nodiscard]] std::size_t active_count() const;

    struct State;

private:
    std::shared_ptr<State> m_state;
    CO_MTA_USAGE_COOKIE m_mta_cookie = nullptr;
};

} // namespace boson::shell
