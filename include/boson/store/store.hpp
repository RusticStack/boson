#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <stdexec/execution.hpp>

namespace boson::store {

/// @brief Failure of a store operation. code is a Win32 error code (GetLastError or one of the ERROR_*
/// values documented below); message is UTF-8 and safe to show to the frontend.
struct StoreError {
    std::uint32_t code = 0;
    std::string message;
};

template <typename T>
using StoreResult = std::expected<T, StoreError>;

/// @brief How the store obtained its value when it was opened (or last reload()ed).
enum class LoadStatus : std::uint8_t {
    Loaded,              ///< the file was read and parsed
    Missing,             ///< no file yet: defaults (T{} + normalize)
    Corrupt,             ///< unreadable JSON / not UTF-8 / empty / too large: backed up, defaults used
    RecoveredFromBackup, ///< the file was corrupt (and backed up), the last good "<file>.bak" was used
    Unreadable,          ///< I/O error other than "not found" (e.g. access denied): defaults, file untouched
    InMemory,            ///< StoreOptions::path is empty: nothing is read or written
};

[[nodiscard]] std::string_view to_string(LoadStatus status) noexcept;

struct LoadReport {
    LoadStatus status = LoadStatus::InMemory;
    /// Where the corrupt file was moved ("<file>.corrupt-<UTC timestamp>"), empty when nothing was backed up.
    std::filesystem::path corrupt_backup;
    /// Leftover "<file>.*.tmp" files of an interrupted save that were deleted while opening.
    std::size_t stale_temp_files_removed = 0;
    /// Parse / I/O error text for Corrupt, RecoveredFromBackup and Unreadable.
    std::string message;
    /// Configuration problems (e.g. a restart key that names no field of T). Empty when all is well.
    std::vector<std::string> warnings;
};

/// @brief Who makes a change, plus an optional last-moment veto. Passed to update / set / patch_json / reset.
struct WriteContext {
    std::string_view source;          ///< empty: "native" (update / set / patch_json) or "reset"
    std::uint64_t sender_webview = 0; ///< reported in StoreChange (the IPC controller passes the caller)
    /// Called with the changed keys after normalize, before anything is saved or committed. An error
    /// rejects the change (the store stays as it was) and is returned by the mutating call.
    std::function<std::expected<void, StoreError>(const std::vector<std::string>& changed_keys)> validate;
};

/// @brief Result of one update / set / patch / reset / reload. Also the IPC response of the Store controller.
struct StoreChange {
    bool changed = false;                     ///< false: the new value equals the old one (nothing written)
    std::vector<std::string> changed_keys;    ///< top-level JSON keys whose value differs (sorted)
    bool restart_required = false;            ///< a changed key is marked "requires restart"
    std::vector<std::string> restart_keys;    ///< the changed keys that require a restart
    std::uint64_t revision = 0;               ///< increments on every committed change (starts at 0)
    bool saved = false;                       ///< the new value is on disk (false: in-memory store, autosave off)
    std::string source;                       ///< "native", "ipc", "reset" or "reload"
    std::uint64_t sender_webview = 0;         ///< WebView::id() of the IPC caller, 0 otherwise
};

struct AtomicWriteOptions {
    /// FlushFileBuffers on the temp file before it replaces the target (survives power loss).
    bool flush = true;
    /// Keep the replaced file as "<file>.bak" (ReplaceFileW backup). Store uses it to recover a corrupt file.
    bool keep_backup = true;
    /// Fault-injection / diagnostics hook: called with the temp file once it is complete (and flushed),
    /// right before ReplaceFileW. A crash here leaves the temp file behind and the original intact.
    std::function<void(const std::filesystem::path& temp_file)> before_replace;
};

/// @brief Writes bytes to "<path>.<pid>-<n>.tmp" next to path, then swaps it in with ReplaceFileW (or
/// MoveFileExW when path does not exist yet). Creates missing parent directories. Readers see either the
/// old or the new file, never a partial one. Retries briefly on sharing violations. Blocking.
[[nodiscard]] StoreResult<void> write_file_atomic(
    const std::filesystem::path& path, std::span<const std::byte> bytes, const AtomicWriteOptions& options = {});

/// @brief %LOCALAPPDATA%\<app_name>\<file_name>. app_name / file_name must be single path components.
[[nodiscard]] StoreResult<std::filesystem::path> default_store_path(
    std::wstring_view app_name, std::wstring_view file_name = L"settings.json");

struct StoreOptions {
    /// The JSON file (see default_store_path). Empty: in-memory store (nothing is read or written).
    std::filesystem::path path;
    /// Name reported in "store:changed" events, to tell several stores apart.
    std::string name = "settings";
    /// Pretty-printed JSON on disk (humans edit settings files).
    bool pretty = true;
    /// true: every committed change is written before update() returns. false: call save() yourself.
    bool autosave = true;
    /// Unknown keys in the file are ignored (older / newer app versions). false: such a file is Corrupt.
    bool ignore_unknown_keys = true;
    /// Write the defaults when the file is missing (so users find a file to edit).
    bool create_if_missing = false;
    /// Files larger than this are treated as corrupt.
    std::size_t max_file_bytes = 16u * 1024u * 1024u;
    /// Corrupt-file backups kept next to the file (oldest are deleted). 0 keeps none.
    std::size_t max_corrupt_backups = 3;
    /// Top-level JSON keys whose change needs an app / browser restart (merged with T::boson_restart_keys).
    std::vector<std::string> restart_keys;
    /// Passed to write_file_atomic for every save (flush, .bak, fault-injection hook).
    AtomicWriteOptions write;
};

/// @brief Value type of a Store: a glaze-reflected (aggregate or glz::meta) object, default-constructible
/// (the defaults) and copyable.
template <typename T>
concept StoreValue = std::default_initializable<T> && std::copyable<T> && (glz::reflectable<T> || glz::glaze_object_t<T>);

template <typename T>
    requires StoreValue<T>
class Store;

/// @brief RAII listener registration of a Store<T> (unsubscribes on destruction / reset()). Keeps the store
/// alive. Declare it after whatever the listener uses (e.g. the Bridge), so it is destroyed first.
template <typename T>
class Subscription {
public:
    Subscription() = default;
    Subscription(std::shared_ptr<Store<T>> store, std::uint64_t id) noexcept : m_store(std::move(store)), m_id(id) {}
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& other) noexcept : m_store(std::move(other.m_store)), m_id(std::exchange(other.m_id, 0)) {}
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            m_store = std::move(other.m_store);
            m_id = std::exchange(other.m_id, 0);
        }
        return *this;
    }
    ~Subscription() { reset(); }

    void reset() noexcept {
        if (m_store && m_id != 0) {
            static_cast<void>(m_store->unsubscribe(m_id));
        }
        m_store.reset();
        m_id = 0;
    }

    [[nodiscard]] bool active() const noexcept { return m_store != nullptr && m_id != 0; }
    [[nodiscard]] std::uint64_t id() const noexcept { return m_id; }

private:
    std::shared_ptr<Store<T>> m_store;
    std::uint64_t m_id = 0;
};

namespace detail {

[[nodiscard]] StoreResult<std::vector<std::string>> diff_top_level(std::string_view old_json, std::string_view new_json);

struct ReadResult {
    enum class Kind : std::uint8_t { Ok, Missing, TooLarge, Error } kind = Kind::Error;
    std::string text;
    StoreError error;
};

/// @brief Reads a whole file (UTF-8 BOM stripped). Missing only for ERROR_FILE_NOT_FOUND / PATH_NOT_FOUND,
/// TooLarge above max_bytes.
[[nodiscard]] ReadResult read_file(const std::filesystem::path& path, std::size_t max_bytes);
/// @brief Moves path to "<path>.corrupt-<UTC>", trims old backups. Falls back to a copy if the move fails.
[[nodiscard]] StoreResult<std::filesystem::path> backup_corrupt(const std::filesystem::path& path, std::size_t keep);
/// @brief Deletes "<path>.*.tmp" leftovers (files still open by a writer fail to delete and are kept).
std::size_t remove_stale_temp_files(const std::filesystem::path& path) noexcept;
[[nodiscard]] std::filesystem::path backup_path_of(const std::filesystem::path& path);
[[nodiscard]] bool valid_utf8(std::string_view text) noexcept;

/// Guards against update() re-entering the same store from its own update function or change listener.
[[nodiscard]] bool is_writing(const void* store) noexcept;
class WriteScope {
public:
    explicit WriteScope(const void* store) noexcept;
    ~WriteScope();
    WriteScope(const WriteScope&) = delete;
    WriteScope& operator=(const WriteScope&) = delete;

private:
    bool m_pushed = false;
};

template <typename T>
concept HasRestartKeys = requires {
    { T::boson_restart_keys.size() } -> std::convertible_to<std::size_t>;
};

template <typename T>
[[nodiscard]] consteval bool is_field_key(std::string_view key) {
    for (const auto k : glz::reflect<T>::keys) {
        if (std::string_view{k} == key) {
            return true;
        }
    }
    return false;
}

template <typename T>
[[nodiscard]] consteval bool restart_keys_valid() {
    if constexpr (HasRestartKeys<T>) {
        for (const auto key : T::boson_restart_keys) {
            if (!is_field_key<T>(std::string_view{key})) {
                return false;
            }
        }
    }
    return true;
}

template <typename T>
[[nodiscard]] bool is_field_key_rt(std::string_view key) noexcept {
    for (const auto k : glz::reflect<T>::keys) {
        if (std::string_view{k} == key) {
            return true;
        }
    }
    return false;
}

template <typename T, typename Fn>
concept NormalizeHook = std::invocable<Fn&, T&>;

template <typename T>
concept AdlNormalize = requires(T& value) { normalize(value); };

} // namespace detail

/// @brief Persistent typed settings (electron-store / tauri-plugin-store for Boson).
///
/// * Defaults come from T{}; a missing key in the file keeps its default, an unknown key is ignored.
/// * Opening tolerates a missing, empty or corrupt file: a corrupt file is moved aside
///   ("<file>.corrupt-<UTC>"), then "<file>.bak" (the previous good save) or the defaults are used.
/// * normalize(T&) runs after every load and every change (clamp ranges, fix enums, migrate values). Pass it
///   to the constructor or declare a free normalize(T&) next to T (found by ADL).
/// * get() / read() are lock-shared and never touch the disk. update() / set() / patch_json() / reset()
///   are serialized, persist with write_file_atomic (temp file + ReplaceFileW) and then notify listeners.
/// * Top-level keys listed in T::boson_restart_keys (a constexpr std::array of names, checked at compile
///   time) or StoreOptions::restart_keys mark a change as StoreChange::restart_required.
///
/// Saving is blocking file I/O: call the mutating functions from a worker thread (the IPC router already
/// does) or use the *_on(scheduler) senders. The constructor reads the file synchronously, which is meant
/// for startup, before settings such as browser arguments are needed.
template <typename T>
    requires StoreValue<T>
class Store {
public:
    using Normalizer = std::function<void(T&)>;
    using Listener = std::function<void(const T& value, const StoreChange& change)>;

    static_assert(detail::restart_keys_valid<T>(), "T::boson_restart_keys names a key that is not a field of T");

    explicit Store(StoreOptions options = {}, Normalizer normalizer = {})
        : m_options(std::move(options)), m_normalizer(std::move(normalizer)) {
        if (!m_normalizer) {
            if constexpr (detail::AdlNormalize<T>) {
                m_normalizer = [](T& value) { normalize(value); };
            }
        }
        if constexpr (detail::HasRestartKeys<T>) {
            for (const auto key : T::boson_restart_keys) {
                add_restart_key(std::string{std::string_view{key}});
            }
        }
        for (const auto& key : m_options.restart_keys) {
            if (detail::is_field_key_rt<T>(key)) {
                add_restart_key(key);
            } else {
                m_report.warnings.push_back("restart key '" + key + "' is not a field of the store type");
            }
        }
        const detail::WriteScope scope(this);
        auto loaded = load_from_disk(m_report);
        m_value = std::move(loaded);
        m_json = serialize_compact(m_value).value_or(std::string{"{}"});
        if (m_report.status == LoadStatus::Missing && m_options.create_if_missing) {
            if (auto saved = write_to_disk(m_value); !saved) {
                m_report.warnings.push_back("could not create the settings file: " + saved.error().message);
            }
        }
    }

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;
    ~Store() = default;

    // ---- reading (any thread, never blocks on disk) ----------------------------------------------------

    /// @brief A copy of the current value.
    [[nodiscard]] T get() const {
        std::shared_lock lock(m_value_mutex);
        return m_value;
    }

    /// @brief Calls fn(const T&) under the shared lock (no copy). fn must not call mutating functions.
    template <typename Fn>
        requires std::invocable<Fn&, const T&>
    decltype(auto) read(Fn&& fn) const {
        std::shared_lock lock(m_value_mutex);
        return std::forward<Fn>(fn)(m_value);
    }

    /// @brief The current value as compact JSON.
    [[nodiscard]] std::string json() const {
        std::shared_lock lock(m_value_mutex);
        return m_json;
    }

    [[nodiscard]] std::uint64_t revision() const {
        std::shared_lock lock(m_value_mutex);
        return m_revision;
    }

    /// @brief true when a change was committed but not saved (autosave off, or the last save failed).
    [[nodiscard]] bool dirty() const {
        std::shared_lock lock(m_value_mutex);
        return m_dirty;
    }

    /// @brief Restart keys changed since this store was opened (the app has not restarted yet).
    [[nodiscard]] std::vector<std::string> pending_restart_keys() const {
        std::shared_lock lock(m_value_mutex);
        return m_pending_restart;
    }

    [[nodiscard]] bool is_restart_key(std::string_view key) const noexcept {
        for (const auto& k : m_restart_keys) {
            if (k == key) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const std::vector<std::string>& restart_keys() const noexcept { return m_restart_keys; }
    [[nodiscard]] const StoreOptions& options() const noexcept { return m_options; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_options.path; }

    /// @brief How the value was obtained by the constructor or the last reload().
    [[nodiscard]] LoadReport load_report() const {
        std::shared_lock lock(m_value_mutex);
        return m_report;
    }

    // ---- writing (serialized; blocking when autosave) -----------------------------------------------------

    /// @brief Applies fn(T&) to a copy of the value, normalizes it and commits it when it changed. fn may
    /// return bool: false cancels. Fails with ERROR_POSSIBLE_DEADLOCK when called from inside fn or a
    /// listener of this store, and with the save error (value unchanged) when autosave cannot write.
    template <typename Fn>
        requires std::invocable<Fn&, T&>
    StoreResult<StoreChange> update(Fn&& fn, const WriteContext& context = {}) {
        return mutate(
            [&fn](T& value) -> StoreResult<bool> {
                using R = std::invoke_result_t<Fn&, T&>;
                if constexpr (std::is_same_v<R, bool>) {
                    return fn(value);
                } else {
                    fn(value);
                    return true;
                }
            },
            context, "native");
    }

    /// @brief Replaces the whole value (normalized).
    StoreResult<StoreChange> set(T value, const WriteContext& context = {}) {
        return mutate(
            [&value](T& current) -> StoreResult<bool> {
                current = std::move(value);
                return true;
            },
            context, "native");
    }

    /// @brief Merges a JSON object into the value: present keys are replaced (nested objects merged,
    /// arrays / maps replaced as glaze reads them), absent keys are kept. Unknown keys fail with
    /// ERROR_INVALID_DATA, as does anything that is not an object.
    StoreResult<StoreChange> patch_json(std::string_view patch, const WriteContext& context = {}) {
        return mutate(
            [patch](T& current) -> StoreResult<bool> {
                const std::size_t first = patch.find_first_not_of(" \t\r\n");
                if (first == std::string_view::npos || patch[first] != '{') {
                    return std::unexpected(StoreError{.code = 13, .message = "patch must be a JSON object"});
                }
                if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true}>(current, patch)) {
                    return std::unexpected(StoreError{.code = 13, .message = "invalid patch: " + glz::format_error(ec, patch)});
                }
                return true;
            },
            context, "native");
    }

    /// @brief Restores the defaults (T{} + normalize).
    StoreResult<StoreChange> reset(const WriteContext& context = {}) {
        return mutate(
            [](T& current) -> StoreResult<bool> {
                current = T{};
                return true;
            },
            context, "reset");
    }

    /// @brief Writes the current value (needed with autosave off; with autosave on it retries a failed save).
    StoreResult<void> save() {
        if (detail::is_writing(this)) {
            return std::unexpected(reentrancy_error());
        }
        std::scoped_lock lock(m_write_mutex);
        const detail::WriteScope scope(this);
        T snapshot = get();
        if (auto saved = write_to_disk(snapshot); !saved) {
            return saved;
        }
        std::unique_lock value_lock(m_value_mutex);
        m_dirty = false;
        return {};
    }

    /// @brief Re-reads the file (e.g. after an external edit), with the same tolerance as the constructor.
    /// Listeners are told when the value changed (source "reload"). Unsaved changes are discarded.
    StoreResult<StoreChange> reload() {
        if (detail::is_writing(this)) {
            return std::unexpected(reentrancy_error());
        }
        std::scoped_lock lock(m_write_mutex);
        const detail::WriteScope scope(this);
        LoadReport report;
        report.warnings = m_report.warnings;
        T loaded = load_from_disk(report);
        {
            std::unique_lock value_lock(m_value_mutex);
            m_report = std::move(report);
        }
        return commit_locked(std::move(loaded), WriteContext{.source = "reload"}, false);
    }

    // ---- notifications ----------------------------------------------------------------------------------

    /// @brief Calls listener after every committed change, on the thread that made it, in commit order.
    /// A listener must not call mutating functions of this store (they fail with ERROR_POSSIBLE_DEADLOCK).
    std::uint64_t subscribe(Listener listener) {
        std::scoped_lock lock(m_listener_mutex);
        const std::uint64_t id = m_next_listener++;
        m_listeners.emplace_back(id, std::make_shared<Listener>(std::move(listener)));
        return id;
    }

    /// @brief Removes a listener. Unless called from inside a notification of this store, it also waits
    /// until a notification running on another thread has finished, so the listener is never called after
    /// unsubscribe() returns (it may wait for a save in progress).
    bool unsubscribe(std::uint64_t id) {
        bool removed = false;
        {
            std::scoped_lock lock(m_listener_mutex);
            for (auto it = m_listeners.begin(); it != m_listeners.end(); ++it) {
                if (it->first == id) {
                    m_listeners.erase(it);
                    removed = true;
                    break;
                }
            }
        }
        if (removed && !detail::is_writing(this)) {
            std::scoped_lock barrier(m_write_mutex); // notifications run under it
        }
        return removed;
    }

    // ---- std::execution ---------------------------------------------------------------------------------

    /// @brief Sender that runs update(fn) on sched and completes with its StoreResult<StoreChange>.
    /// The store must outlive the operation.
    template <stdexec::scheduler Sched, typename Fn>
        requires std::invocable<Fn&, T&>
    [[nodiscard]] auto update_on(Sched sched, Fn fn, std::string source = "native") {
        return stdexec::schedule(std::move(sched)) |
               stdexec::then([this, fn = std::move(fn), source = std::move(source)]() mutable {
                   return update(fn, WriteContext{.source = source});
               });
    }

    /// @brief Sender that runs save() on sched.
    template <stdexec::scheduler Sched>
    [[nodiscard]] auto save_on(Sched sched) {
        return stdexec::schedule(std::move(sched)) | stdexec::then([this]() { return save(); });
    }

private:
    static StoreError reentrancy_error() {
        return StoreError{.code = 1131, .message = "store modified from inside its own update function or change listener"};
    }

    void add_restart_key(std::string key) {
        if (!is_restart_key(key)) {
            m_restart_keys.push_back(std::move(key));
        }
    }

    static StoreResult<std::string> serialize_compact(const T& value) {
        std::string out;
        if (const auto ec = glz::write_json(value, out)) {
            return std::unexpected(StoreError{.code = 13, .message = "cannot serialize settings: " + glz::format_error(ec, out)});
        }
        return out;
    }

    void normalize_value(T& value) const {
        if (m_normalizer) {
            m_normalizer(value);
        }
    }

    StoreResult<T> parse(std::string_view text) const {
        T value{};
        if (!detail::valid_utf8(text)) {
            return std::unexpected(StoreError{.code = 13, .message = "file is not valid UTF-8"});
        }
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            return std::unexpected(StoreError{.code = 13, .message = "file is empty"});
        }
        glz::error_ctx ec{};
        if (m_options.ignore_unknown_keys) {
            ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(value, text);
        } else {
            ec = glz::read<glz::opts{.error_on_unknown_keys = true}>(value, text);
        }
        if (ec) {
            return std::unexpected(StoreError{.code = 13, .message = glz::format_error(ec, text)});
        }
        return value;
    }

    // Runs with m_write_mutex held (or from the constructor). Never fails: falls back to defaults.
    T load_from_disk(LoadReport& report) const {
        report.corrupt_backup.clear();
        report.message.clear();
        report.stale_temp_files_removed = 0;
        T value{};
        if (m_options.path.empty()) {
            report.status = LoadStatus::InMemory;
            normalize_value(value);
            return value;
        }
        report.stale_temp_files_removed = detail::remove_stale_temp_files(m_options.path);
        auto file = detail::read_file(m_options.path, m_options.max_file_bytes);
        if (file.kind == detail::ReadResult::Kind::Missing) {
            report.status = LoadStatus::Missing;
        } else if (file.kind == detail::ReadResult::Kind::Error) {
            report.status = LoadStatus::Unreadable;
            report.message = std::move(file.error.message);
        } else {
            auto parsed = file.kind == detail::ReadResult::Kind::Ok
                ? parse(file.text)
                : StoreResult<T>{std::unexpected(std::move(file.error))};
            if (parsed) {
                report.status = LoadStatus::Loaded;
                value = std::move(*parsed);
            } else {
                report.status = LoadStatus::Corrupt;
                report.message = std::move(parsed.error().message);
                if (auto moved = detail::backup_corrupt(m_options.path, m_options.max_corrupt_backups)) {
                    report.corrupt_backup = std::move(*moved);
                } else {
                    report.warnings.push_back("could not back up the corrupt file: " + moved.error().message);
                }
                auto bak = detail::read_file(detail::backup_path_of(m_options.path), m_options.max_file_bytes);
                if (bak.kind == detail::ReadResult::Kind::Ok) {
                    if (auto recovered = parse(bak.text)) {
                        report.status = LoadStatus::RecoveredFromBackup;
                        value = std::move(*recovered);
                    }
                }
            }
        }
        normalize_value(value);
        return value;
    }

    StoreResult<void> write_to_disk(const T& value) const {
        if (m_options.path.empty()) {
            return {};
        }
        std::string text;
        glz::error_ctx ec{};
        if (m_options.pretty) {
            ec = glz::write<glz::opts{.prettify = true}>(value, text);
            text.push_back('\n');
        } else {
            ec = glz::write_json(value, text);
        }
        if (ec) {
            return std::unexpected(StoreError{.code = 13, .message = "cannot serialize settings: " + glz::format_error(ec, text)});
        }
        return write_file_atomic(m_options.path, std::as_bytes(std::span<const char>(text.data(), text.size())), m_options.write);
    }

    template <typename Mutator>
    StoreResult<StoreChange> mutate(Mutator&& mutator, const WriteContext& context, std::string_view default_source) {
        if (detail::is_writing(this)) {
            return std::unexpected(reentrancy_error());
        }
        std::scoped_lock lock(m_write_mutex);
        const detail::WriteScope scope(this);
        T next = get();
        auto proceed = mutator(next);
        if (!proceed) {
            return std::unexpected(std::move(proceed.error()));
        }
        WriteContext ctx{
            .source = context.source.empty() ? default_source : context.source,
            .sender_webview = context.sender_webview,
            .validate = context.validate,
        };
        if (!*proceed) {
            StoreChange none;
            none.revision = revision();
            none.source = std::string{ctx.source};
            none.sender_webview = ctx.sender_webview;
            return none;
        }
        normalize_value(next);
        return commit_locked(std::move(next), ctx, m_options.autosave);
    }

    // m_write_mutex held.
    StoreResult<StoreChange> commit_locked(T next, const WriteContext& context, bool save) {
        const std::string_view source = context.source;
        auto next_json = serialize_compact(next);
        if (!next_json) {
            return std::unexpected(std::move(next_json.error()));
        }
        StoreChange change;
        change.source = std::string{source};
        change.sender_webview = context.sender_webview;
        {
            std::shared_lock value_lock(m_value_mutex);
            change.revision = m_revision;
            if (*next_json == m_json) {
                return change;
            }
            auto keys = detail::diff_top_level(m_json, *next_json);
            if (!keys) {
                return std::unexpected(std::move(keys.error()));
            }
            change.changed_keys = std::move(*keys);
        }
        change.changed = true;
        for (const auto& key : change.changed_keys) {
            if (is_restart_key(key)) {
                change.restart_keys.push_back(key);
            }
        }
        change.restart_required = !change.restart_keys.empty();
        if (context.validate) {
            if (auto verdict = context.validate(change.changed_keys); !verdict) {
                return std::unexpected(std::move(verdict.error()));
            }
        }
        const bool persistent = !m_options.path.empty();
        if (save && persistent) {
            if (auto saved = write_to_disk(next); !saved) {
                return std::unexpected(std::move(saved.error()));
            }
            change.saved = true;
        }
        {
            std::unique_lock value_lock(m_value_mutex);
            m_value = next;
            m_json = std::move(*next_json);
            change.revision = ++m_revision;
            // reload() reads what is on disk, so it never leaves the store dirty.
            m_dirty = persistent && !change.saved && source != "reload";
            for (const auto& key : change.restart_keys) {
                bool known = false;
                for (const auto& pending : m_pending_restart) {
                    known = known || pending == key;
                }
                if (!known) {
                    m_pending_restart.push_back(key);
                }
            }
        }
        notify(next, change);
        return change;
    }

    // m_write_mutex held: listeners see changes in commit order.
    void notify(const T& value, const StoreChange& change) const {
        std::vector<std::shared_ptr<Listener>> listeners;
        {
            std::scoped_lock lock(m_listener_mutex);
            listeners.reserve(m_listeners.size());
            for (const auto& entry : m_listeners) {
                listeners.push_back(entry.second);
            }
        }
        if (listeners.empty()) {
            return;
        }
        for (const auto& listener : listeners) {
            try {
                (*listener)(value, change);
            } catch (...) {
                // A throwing listener must not break the store or the other listeners.
            }
        }
    }

    StoreOptions m_options;
    Normalizer m_normalizer;
    std::vector<std::string> m_restart_keys;

    mutable std::mutex m_write_mutex;          // serializes writers, saving and notification
    mutable std::shared_mutex m_value_mutex;   // guards the fields below
    T m_value{};
    std::string m_json;
    std::uint64_t m_revision = 0;
    bool m_dirty = false;
    std::vector<std::string> m_pending_restart;
    LoadReport m_report;

    mutable std::mutex m_listener_mutex;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<Listener>>> m_listeners;
    std::uint64_t m_next_listener = 1;
};

} // namespace boson::store
