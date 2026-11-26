#pragma once

#include <boson/core/result.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/export.hpp>
#include <boson/ipc/request_context.hpp>
#include <boson/ipc/ts_generator.hpp>
#include <boson/store/store.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boson::ipc {

/// @brief A string literal usable as a template argument: StoreController<Settings, "Prefs">.
template <std::size_t N>
struct ControllerName {
    std::array<char, N> chars{};

    consteval ControllerName(const char (&text)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            chars[i] = text[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept { return {chars.data(), N - 1}; }
};

/// @brief Params of Store.patch: a JSON object with some of T's keys. TypeScript: Partial<T>.
template <typename T>
struct StorePatch {
    using boson_ts_partial = T;
    glz::raw_json json;
};

/// @brief Response of Store.info.
struct StoreInfo {
    std::string name;
    std::string path;           ///< UTF-8, empty for an in-memory store
    std::string load_status;    ///< store::to_string(LoadStatus)
    std::uint64_t revision = 0;
    bool dirty = false;
    std::vector<std::string> restart_keys;          ///< keys that require a restart when they change
    std::vector<std::string> pending_restart_keys;  ///< restart keys changed since the app started
    std::vector<std::string> read_only_keys;        ///< keys the frontend may not change
};

/// @brief Payload of the "store:changed" event.
template <typename T>
struct StoreChangedEvent {
    std::string store;          ///< StoreOptions::name
    T value;
    store::StoreChange change;
};

/// @brief Event map for TsGenerator::generate_with_events (types the "store:changed" payload as T).
template <typename T>
struct StoreEvents {
    StoreChangedEvent<T> store_changed;
};

struct StoreControllerOptions {
    /// Top-level keys the frontend may not change through set / patch / reset (C++ still can). A request
    /// that would change one fails; a reset keeps their current values.
    std::vector<std::string> read_only_keys;
};

/// @brief JS controller over a store::Store<T> (default name "Store"):
///   Store.get() -> T, Store.set(T) / Store.patch(Partial<T>) / Store.reset() -> StoreChange, Store.info().
/// Handlers run on the router's pool thread (the save never blocks the UI thread). Permissions go through
/// the Bridge's CapabilityPolicy as usual ("Store.get", "Store.set", "Store.*"); read_only_keys adds a
/// per-key guard on top. connect_store_events() forwards every committed change - from C++ or from any
/// WebView - as "store:changed" (StoreChangedEvent<T>) while the returned subscription lives.
template <typename T, ControllerName Name = "Store">
    requires store::StoreValue<T>
class StoreController {
public:
    BOSON_CONTROLLER(Name.view());

    explicit StoreController(std::shared_ptr<store::Store<T>> store, StoreControllerOptions options = {})
        : m_store(store ? std::move(store) : std::make_shared<store::Store<T>>()), m_options(std::move(options)) {}

    StoreController(const StoreController&) = delete;
    StoreController& operator=(const StoreController&) = delete;

    ~StoreController() = default;

    [[nodiscard]] T get() const { return m_store->get(); }

    [[nodiscard]] store::StoreChange set(T value) const {
        return unwrap(m_store->set(std::move(value), context()));
    }

    [[nodiscard]] store::StoreChange patch(StorePatch<T> req) const {
        return unwrap(m_store->patch_json(req.json.str, context()));
    }

    /// @brief Restores the defaults; read-only keys keep their current values.
    [[nodiscard]] store::StoreChange reset() const {
        if (m_options.read_only_keys.empty()) {
            return unwrap(m_store->reset(context()));
        }
        store::WriteContext ctx = context();
        ctx.source = "reset";
        return unwrap(m_store->update(
            [this](T& value) {
                const std::string keep = read_only_fields(value);
                value = T{};
                static_cast<void>(glz::read<glz::opts{.error_on_unknown_keys = false}>(value, keep));
            },
            ctx));
    }

    [[nodiscard]] StoreInfo info() const {
        const store::LoadReport report = m_store->load_report();
        return StoreInfo{
            .name = m_store->options().name,
            .path = core::wide_to_utf8(m_store->path().wstring()),
            .load_status = std::string{store::to_string(report.status)},
            .revision = m_store->revision(),
            .dirty = m_store->dirty(),
            .restart_keys = m_store->restart_keys(),
            .pending_restart_keys = m_store->pending_restart_keys(),
            .read_only_keys = m_options.read_only_keys,
        };
    }

    // Not IPC methods: hidden from the router (friends are not members).

    /// @brief Emits every committed change of the store (from C++ or any WebView) as event (default
    /// "store:changed", payload StoreChangedEvent<T>) through bridge, until the returned subscription is
    /// destroyed. Declare it after the Bridge so it goes first.
    [[nodiscard]] friend store::Subscription<T> connect_store_events(
        StoreController& controller, Bridge& bridge, std::string event = "store:changed") {
        const std::string name = controller.m_store->options().name;
        const std::uint64_t id = controller.m_store->subscribe(
            [&bridge, event = std::move(event), name](const T& value, const store::StoreChange& change) {
                bridge.emit(event, StoreChangedEvent<T>{.store = name, .value = value, .change = change});
            });
        return store::Subscription<T>(controller.m_store, id);
    }

    friend store::Store<T>& store_of(StoreController& controller) noexcept { return *controller.m_store; }

private:
    [[nodiscard]] bool is_read_only(std::string_view key) const noexcept {
        return std::ranges::find(m_options.read_only_keys, key) != m_options.read_only_keys.end();
    }

    [[nodiscard]] store::WriteContext context() const {
        store::WriteContext ctx{.source = "ipc", .sender_webview = current_sender_id(), .validate = {}};
        if (!m_options.read_only_keys.empty()) {
            ctx.validate = [this](const std::vector<std::string>& changed) -> std::expected<void, store::StoreError> {
                for (const auto& key : changed) {
                    if (is_read_only(key)) {
                        return std::unexpected(store::StoreError{
                            .code = 5, // ERROR_ACCESS_DENIED
                            .message = std::string{Name.view()} + ": '" + key + "' is read-only"});
                    }
                }
                return {};
            };
        }
        return ctx;
    }

    /// {"key":value,...} holding value's read-only keys.
    [[nodiscard]] std::string read_only_fields(const T& value) const {
        std::string all;
        std::map<std::string, glz::raw_json> fields;
        if (glz::write_json(value, all) || glz::read_json(fields, all)) {
            return "{}";
        }
        std::map<std::string, glz::raw_json> kept;
        for (const auto& key : m_options.read_only_keys) {
            if (const auto it = fields.find(key); it != fields.end()) {
                kept.emplace(it->first, it->second);
            }
        }
        std::string out;
        if (glz::write_json(kept, out)) {
            return "{}";
        }
        return out;
    }

    static store::StoreChange unwrap(store::StoreResult<store::StoreChange> result) {
        if (!result) {
            throw std::runtime_error(result.error().message);
        }
        return std::move(*result);
    }

    std::shared_ptr<store::Store<T>> m_store;
    StoreControllerOptions m_options;
};

} // namespace boson::ipc

template <typename T>
struct glz::meta<boson::ipc::StorePatch<T>> {
    static constexpr auto value = &boson::ipc::StorePatch<T>::json;
};

template <typename T>
struct glz::meta<boson::ipc::StoreEvents<T>> {
    using E = boson::ipc::StoreEvents<T>;
    static constexpr auto value = glz::object("store:changed", &E::store_changed);
};

template <typename T, boson::ipc::ControllerName Name>
struct glz::meta<boson::ipc::StoreController<T, Name>> {
    using C = boson::ipc::StoreController<T, Name>;
    static constexpr auto value = glz::object(
        "get", &C::get,
        "set", &C::set,
        "patch", &C::patch,
        "reset", &C::reset,
        "info", &C::info);
};
