#pragma once

#include <boson/app/lifecycle.hpp>

#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::desktop {

/// @brief RFC 3986 scheme syntax (ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )), at least 2 characters (a
/// single letter would be a drive letter), and not a reserved scheme Windows or browsers own
/// (http, https, file, ftp, mailto, javascript, data, ms-*, ...).
[[nodiscard]] bool is_valid_custom_scheme(std::string_view scheme) noexcept;

struct ProtocolClientOptions {
    /// Executable to launch; empty = the running executable (app::executable_path()).
    std::wstring executable;
    /// Placed before "%1" on the command line.
    std::vector<std::wstring> arguments;
    /// Shown by Windows in "Open with" prompts; default "URL:<scheme>".
    std::wstring description;
    /// DefaultIcon value; default "<executable>,0".
    std::wstring icon;
};

/// @brief Writes HKCU\Software\Classes\<scheme> (no elevation, current user only):
///   (Default) = "URL:<description>", "URL Protocol" = "", DefaultIcon, shell\open\command =
///   "<exe>" <arguments...> "%1", then SHChangeNotify(SHCNE_ASSOCCHANGED).
[[nodiscard]] std::expected<void, std::string> register_protocol_client(
    std::string_view scheme,
    const ProtocolClientOptions& options = {});

/// @brief Deletes HKCU\Software\Classes\<scheme>. Unless force is set, only when it is registered to
/// executable (empty = this executable), so another app's registration is never removed by accident.
/// Succeeds when nothing is registered.
[[nodiscard]] std::expected<void, std::string> unregister_protocol_client(
    std::string_view scheme,
    std::wstring_view executable = {},
    bool force = false);

/// @brief true when launching <scheme>: URLs starts executable (empty = this executable): the per-user
/// UrlAssociations UserChoice ProgId when one exists, else the merged HKCR\<scheme> registration.
[[nodiscard]] bool is_default_protocol_client(std::string_view scheme, std::wstring_view executable = {});

/// @brief The shell\open\command of the handler Windows would use for scheme, empty when none.
[[nodiscard]] std::wstring protocol_handler_command(std::string_view scheme);
/// @brief First token (the program) of a command line, unquoted.
[[nodiscard]] std::wstring command_program(std::wstring_view command_line);

/// @brief Every argument that is a <scheme>: URL (scheme compared case-insensitively), in order, as UTF-8.
/// Works for argv of this process (app::command_line_args()) and for argv forwarded by SingleInstance.
[[nodiscard]] std::vector<std::string> extract_deep_links(std::span<const std::wstring> args, std::string_view scheme);

/// @brief Wraps a deep-link callback as a SingleInstance::SecondInstanceFn: a second launch with links
/// for scheme calls on_links(links) on the primary's UI thread; a launch without links calls
/// on_other_launch(args, cwd) when provided.
[[nodiscard]] app::SingleInstance::SecondInstanceFn deep_link_forwarder(
    std::string scheme,
    std::function<void(std::vector<std::string> links)> on_links,
    app::SingleInstance::SecondInstanceFn on_other_launch = {});

} // namespace boson::desktop
