#include "boson/compat/electron_ipc.hpp"

#include <windows.h>

#include <algorithm>
#include <exception>
#include <mutex>
#include <utility>

#include "boson/ipc/protocol_types.hpp"

namespace boson::compat {

namespace detail {

// External linkage: glaze's reflection cannot name types from an anonymous namespace.
struct ChannelParams {
    std::string channel;
    glz::raw_json args{};
};

struct EmitParams {
    std::string event;
    glz::raw_json payload{};
    bool broadcast = true;
};

} // namespace detail

namespace {

using detail::ChannelParams;
using detail::EmitParams;

constexpr glz::opts kLenient{.error_on_unknown_keys = false};

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    const auto is_space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// The args member as a JSON array view; empty view when it is present but not an array.
[[nodiscard]] std::string_view args_array(const glz::raw_json& args) noexcept {
    const std::string_view text = trim(args.str);
    if (text.empty() || text == "null") {
        return "[]";
    }
    return text.front() == '[' ? text : std::string_view{};
}

void log_listener_failure(std::string_view name, std::string_view what) noexcept {
    try {
        std::string message = "[Boson] ElectronIpc listener for '";
        message.append(name);
        message.append("' threw: ");
        message.append(what);
        message.push_back('\n');
        ::OutputDebugStringA(message.c_str());
    } catch (...) {
        ::OutputDebugStringA("[Boson] ElectronIpc listener threw\n");
    }
}

} // namespace

std::string ipc_event_name(std::string_view channel) {
    std::string name;
    name.reserve(kIpcEventPrefix.size() + channel.size());
    name.append(kIpcEventPrefix);
    name.append(channel);
    return name;
}

bool ElectronIpc::add_handler(std::string channel, std::shared_ptr<const HandlerEntry> entry) {
    if (channel.empty()) {
        return false;
    }
    const std::unique_lock lock(m_mutex);
    return m_handlers.try_emplace(std::move(channel), std::move(entry)).second;
}

bool ElectronIpc::handle(std::string channel, Handler handler) {
    if (!handler) {
        return false;
    }
    return add_handler(std::move(channel), std::make_shared<const HandlerEntry>(HandlerEntry{.handler = std::move(handler), .raw = {}, .once = false}));
}

bool ElectronIpc::handle_raw(std::string channel, RawHandler handler) {
    if (!handler) {
        return false;
    }
    return add_handler(std::move(channel), std::make_shared<const HandlerEntry>(HandlerEntry{.handler = {}, .raw = std::move(handler), .once = false}));
}

bool ElectronIpc::handle_once(std::string channel, Handler handler) {
    if (!handler) {
        return false;
    }
    return add_handler(std::move(channel), std::make_shared<const HandlerEntry>(HandlerEntry{.handler = std::move(handler), .raw = {}, .once = true}));
}

bool ElectronIpc::remove_handler(std::string_view channel) {
    const std::unique_lock lock(m_mutex);
    const auto it = m_handlers.find(channel);
    if (it == m_handlers.end()) {
        return false;
    }
    m_handlers.erase(it);
    return true;
}

bool ElectronIpc::has_handler(std::string_view channel) const {
    const std::shared_lock lock(m_mutex);
    return m_handlers.find(channel) != m_handlers.end();
}

ElectronIpc::ListenerId ElectronIpc::add_listener(ListenerKind kind, std::string name, Listener listener, bool once) {
    if (!listener || name.empty()) {
        return 0;
    }
    auto shared = std::make_shared<const Listener>(std::move(listener));
    const std::unique_lock lock(m_mutex);
    const ListenerId id = m_next_listener_id++;
    m_listeners.push_back(ListenerEntry{.id = id, .kind = kind, .name = std::move(name), .once = once, .listener = std::move(shared)});
    return id;
}

ElectronIpc::ListenerId ElectronIpc::on(std::string channel, Listener listener) {
    return add_listener(ListenerKind::Channel, std::move(channel), std::move(listener), false);
}

ElectronIpc::ListenerId ElectronIpc::once(std::string channel, Listener listener) {
    return add_listener(ListenerKind::Channel, std::move(channel), std::move(listener), true);
}

ElectronIpc::ListenerId ElectronIpc::on_event(std::string event, Listener listener) {
    return add_listener(ListenerKind::Event, std::move(event), std::move(listener), false);
}

bool ElectronIpc::remove_listener(ListenerId id) {
    const std::unique_lock lock(m_mutex);
    return std::erase_if(m_listeners, [id](const ListenerEntry& entry) { return entry.id == id; }) != 0;
}

void ElectronIpc::remove_all_listeners(std::string_view channel) {
    const std::unique_lock lock(m_mutex);
    if (channel.empty()) {
        m_listeners.clear();
        return;
    }
    std::erase_if(m_listeners, [channel](const ListenerEntry& entry) { return entry.name == channel; });
}

std::size_t ElectronIpc::listener_count(std::string_view channel) const {
    const std::shared_lock lock(m_mutex);
    return static_cast<std::size_t>(std::ranges::count_if(m_listeners, [channel](const ListenerEntry& entry) {
        return entry.kind == ListenerKind::Channel && entry.name == channel;
    }));
}

bool ElectronIpc::send_json(std::uint64_t webview_id, std::string_view channel, std::string_view args_array_json) {
    ipc::Bridge* const target = bridge();
    if (target == nullptr || channel.empty()) {
        return false;
    }
    try {
        return target->emit_json(webview_id, ipc_event_name(channel), args_array_json.empty() ? "[]" : args_array_json);
    } catch (...) {
        return false;
    }
}

bool ElectronIpc::boson_dispatch(
    std::string_view method,
    std::uint64_t request_id,
    std::string_view params_json,
    std::string& out_response) noexcept {
    try {
        if (method == "invoke") {
            out_response = invoke(request_id, params_json);
        } else if (method == "send") {
            out_response = send_from_renderer(request_id, params_json);
        } else if (method == "emit") {
            out_response = emit_from_renderer(request_id, params_json);
        } else {
            return false;
        }
    } catch (const std::exception& ex) {
        try {
            out_response = ipc::make_error_envelope(request_id, -32000, ex.what());
        } catch (...) {
            out_response.clear();
        }
    } catch (...) {
        try {
            out_response = ipc::make_error_envelope(request_id, -32000, "Unknown C++ exception in ElectronIpc");
        } catch (...) {
            out_response.clear();
        }
    }
    return true;
}

std::string ElectronIpc::invoke(std::uint64_t request_id, std::string_view params_json) {
    ChannelParams params{};
    if (const auto ec = glz::read<kLenient>(params, params_json)) {
        return ipc::make_error_envelope(request_id, -32602, glz::format_error(ec, params_json));
    }
    if (params.channel.empty()) {
        return ipc::make_error_envelope(request_id, -32602, "ElectronIpc.invoke: 'channel' is required");
    }
    const std::string_view args = args_array(params.args);
    if (args.empty()) {
        return ipc::make_error_envelope(request_id, -32602, "ElectronIpc.invoke: 'args' must be an array");
    }

    std::shared_ptr<const HandlerEntry> entry;
    {
        const std::shared_lock lock(m_mutex);
        if (const auto it = m_handlers.find(params.channel); it != m_handlers.end()) {
            entry = it->second;
        }
    }
    if (entry && entry->once) {
        // Only the caller that actually removes the once-handler gets to run it.
        const std::unique_lock lock(m_mutex);
        const auto it = m_handlers.find(params.channel);
        if (it != m_handlers.end() && it->second == entry) {
            m_handlers.erase(it);
        } else {
            entry.reset();
        }
    }
    if (!entry) {
        return ipc::make_error_envelope(request_id, -32601, "No handler registered for '" + params.channel + "'");
    }

    const IpcEvent event{.sender_id = ipc::current_sender_id(), .channel = params.channel, .ipc = this};
    try {
        if (entry->raw) {
            std::string result = entry->raw(event, args);
            if (trim(result).empty()) {
                return ipc::make_success_envelope(request_id, "null");
            }
            if (const auto ec = glz::validate_json(result)) {
                return ipc::make_error_envelope(
                    request_id, -32603, "Handler for '" + params.channel + "' returned invalid JSON: " + glz::format_error(ec, result));
            }
            return ipc::make_success_envelope(request_id, result);
        }

        Json parsed{};
        if (const auto ec = glz::read_json(parsed, args)) {
            return ipc::make_error_envelope(request_id, -32602, glz::format_error(ec, args));
        }
        const Json result = entry->handler(event, std::move(parsed));
        std::string data;
        if (const auto ec = glz::write_json(result, data)) {
            return ipc::make_error_envelope(request_id, -32603, glz::format_error(ec, data));
        }
        return ipc::make_success_envelope(request_id, data);
    } catch (const std::exception& ex) {
        return ipc::make_error_envelope(
            request_id, -32000, "Error invoking remote method '" + params.channel + "': " + ex.what());
    } catch (...) {
        return ipc::make_error_envelope(
            request_id, -32000, "Error invoking remote method '" + params.channel + "': unknown C++ exception");
    }
}

std::string ElectronIpc::send_from_renderer(std::uint64_t request_id, std::string_view params_json) {
    ChannelParams params{};
    if (const auto ec = glz::read<kLenient>(params, params_json)) {
        return ipc::make_error_envelope(request_id, -32602, glz::format_error(ec, params_json));
    }
    if (params.channel.empty()) {
        return ipc::make_error_envelope(request_id, -32602, "ElectronIpc.send: 'channel' is required");
    }
    const std::string_view args = args_array(params.args);
    if (args.empty()) {
        return ipc::make_error_envelope(request_id, -32602, "ElectronIpc.send: 'args' must be an array");
    }
    notify_listeners(ListenerKind::Channel, params.channel, args);
    return ipc::make_success_envelope(request_id, "null");
}

std::string ElectronIpc::emit_from_renderer(std::uint64_t request_id, std::string_view params_json) {
    EmitParams params{};
    if (const auto ec = glz::read<kLenient>(params, params_json)) {
        return ipc::make_error_envelope(request_id, -32602, glz::format_error(ec, params_json));
    }
    if (params.event.empty()) {
        return ipc::make_error_envelope(request_id, -32602, "ElectronIpc.emit: 'event' is required");
    }
    std::string_view payload = trim(params.payload.str);
    if (payload.empty()) {
        payload = "null";
    }
    std::string args;
    args.reserve(payload.size() + 2);
    args.push_back('[');
    args.append(payload);
    args.push_back(']');
    notify_listeners(ListenerKind::Event, params.event, args);

    if (params.broadcast) {
        if (ipc::Bridge* const target = bridge(); target != nullptr) {
            target->emit_json(ipc::Bridge::kAllWebViews, params.event, payload);
        }
    }
    return ipc::make_success_envelope(request_id, "null");
}

void ElectronIpc::notify_listeners(ListenerKind kind, std::string_view name, std::string_view args_json) {
    std::vector<std::shared_ptr<const Listener>> targets;
    {
        const std::unique_lock lock(m_mutex);
        for (const auto& entry : m_listeners) {
            if (entry.kind == kind && entry.name == name) {
                targets.push_back(entry.listener);
            }
        }
        std::erase_if(m_listeners, [kind, name](const ListenerEntry& entry) {
            return entry.once && entry.kind == kind && entry.name == name;
        });
    }
    if (targets.empty()) {
        return;
    }

    Json args{};
    if (glz::read_json(args, args_json)) {
        log_listener_failure(name, "arguments are not valid JSON");
        return;
    }
    const IpcEvent event{.sender_id = ipc::current_sender_id(), .channel = name, .ipc = this};
    for (const auto& listener : targets) {
        try {
            (*listener)(event, args);
        } catch (const std::exception& ex) {
            log_listener_failure(name, ex.what());
        } catch (...) {
            log_listener_failure(name, "unknown C++ exception");
        }
    }
}

} // namespace boson::compat
