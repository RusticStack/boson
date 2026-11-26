# Settings store

`boson::store::Store<T>` is Boson's version of `electron-store` / `tauri-plugin-store`: a typed, glaze-reflected
settings struct persisted as JSON, with defaults, tolerant loading, normalisation, thread-safe updates, atomic
saves, change listeners and "requires restart" keys. `ipc::StoreController<T>` exposes it to the page as
`Store.get / set / patch / reset / info` plus a `store:changed` event. The typed C++ API comes first; the
controller is a thin layer over it.

Headers: `<boson/store/store.hpp>`, `<boson/ipc/controllers/store_controller.hpp>` (both are in `<boson/boson.hpp>`).

## 1. Define the settings

```cpp
struct Settings {
    std::string screenshare_mode = "auto";   // defaults come from T{}
    bool hardware_acceleration = true;
    std::string search_engine = "duckduckgo";
    int tab_sleep_minutes = 15;
    std::vector<std::string> browser_args;    // any glaze-serialisable type works

    // Top-level keys whose change needs Environment::recreate() / app::relaunch(). Checked at compile time:
    // a name that is not a field of Settings is a static_assert failure.
    static constexpr std::array<std::string_view, 2> boson_restart_keys{"hardware_acceleration", "browser_args"};
};

// Optional, found by ADL (or pass a std::function<void(Settings&)> to the constructor instead). Runs after
// every load and every change: clamp ranges, repair enums, migrate old values.
void normalize(Settings& s) {
    s.tab_sleep_minutes = std::clamp(s.tab_sleep_minutes, 0, 24 * 60);
}
```

`T` must be default-constructible, copyable and a glaze object (a plain aggregate, or a type with
`glz::meta`; renamed keys from `glz::meta` are what the file, the change keys and the restart keys use).

## 2. Open it

```cpp
auto path = boson::store::default_store_path(L"My App");          // %LOCALAPPDATA%\My App\settings.json
auto settings = std::make_shared<boson::store::Store<Settings>>(boson::store::StoreOptions{
    .path = path.value_or(std::filesystem::path{}),                   // empty path = in-memory store
});
if (!settings->get().hardware_acceleration) {
    env_cfg.additional_browser_arguments.emplace_back(L"--disable-gpu");
}
```

The constructor reads the file synchronously (meant for startup, before browser arguments are needed) and never
fails. `load_report()` says what happened:

| `LoadStatus` | When | Value |
|---|---|---|
| `Loaded` | the file parsed | file content; missing keys keep defaults, unknown keys are ignored (`ignore_unknown_keys`) |
| `Missing` | no file (`create_if_missing` writes the defaults) | `T{}` |
| `Corrupt` | invalid JSON, wrong types, empty, not UTF-8, larger than `max_file_bytes` | `T{}`; the file is moved to `<file>.corrupt-<UTC timestamp>` (at most `max_corrupt_backups` are kept) |
| `RecoveredFromBackup` | corrupt, but `<file>.bak` (the previous good save) parses | the `.bak` content |
| `Unreadable` | I/O error other than "not found" (access denied, ...) | `T{}`; the file is left untouched |
| `InMemory` | `StoreOptions::path` is empty | `T{}` |

`normalize` runs in every case. Leftover `<file>.*.tmp` files of an interrupted save are deleted while opening
(`stale_temp_files_removed`).

## 3. Read and change

```cpp
Settings now = settings->get();                                        // copy, shared lock, no disk access
int minutes = settings->read([](const Settings& s) { return s.tab_sleep_minutes; }); // no copy

auto change = settings->update([](Settings& s) { s.search_engine = "kagi"; });      // blocking save
if (change && change->restart_required) { /* offer app::relaunch() */ }
settings->patch_json(R"({"tab_sleep_minutes": 30})");                 // merge an object
settings->set(Settings{});                                             // replace everything
settings->reset();                                                     // T{} + normalize
```

* Every mutation copies the value, applies the change, normalises, compares the new JSON with the old one per
  top-level key and, only when something changed, saves and commits it. `StoreChange` reports `changed`,
  `changed_keys`, `restart_required`, `restart_keys`, `revision`, `saved`, `source` and `sender_webview`.
* An update function may return `bool`; `false` cancels. `WriteContext::validate` can veto a change by its keys
  after normalisation (the controller uses this for read-only keys).
* With `autosave` (default) the change is written before the call returns; if the save fails the call returns the
  error and the value stays as it was. With `autosave = false` the change is committed in memory, `dirty()` is true
  and `save()` writes it.
* `reload()` re-reads the file (external edits) with the same tolerance as the constructor.
* `pending_restart_keys()` lists restart keys changed since the store was opened.
* Writers are serialised by one mutex; readers use a shared mutex that is only held for the in-memory swap, not
  during disk I/O. Calling a mutating function from inside an update function or a listener of the same store
  returns `ERROR_POSSIBLE_DEADLOCK` (1131) instead of deadlocking.
* Saving is blocking. Call mutations from a worker (IPC handlers already run on the router's pool) or use the
  senders: `stdexec::sync_wait(settings->update_on(scheduler.get_scheduler(), fn))`, `save_on(sched)`.

### Listeners

```cpp
auto id = settings->subscribe([](const Settings& value, const boson::store::StoreChange& change) { ... });
settings->unsubscribe(id);   // or boson::store::Subscription<T> (RAII), as returned by connect_store_events
```

Listeners run on the thread that made the change, in commit order (they run under the writer lock), after the
value is saved. A throwing listener is ignored. `unsubscribe` waits for a notification running on another thread, so a
listener is never called after it returns.

### Atomic writes

`write_file_atomic(path, bytes, AtomicWriteOptions)` is public: it writes `<path>.<pid>-<n>.tmp` in the same folder
(`CREATE_NEW`, no sharing), `FlushFileBuffers` (`flush`), then `ReplaceFileW` with `<path>.bak` as backup
(`keep_backup`) or `MoveFileExW` when the target does not exist yet. ReplaceFileW keeps the target's ACL and
attributes. Sharing violations (antivirus, indexers) are retried for about half a second; the 1176 / 1177 partial
failures of ReplaceFileW are finished with `MoveFileExW`. `before_replace` is a fault-injection hook called between the
flushed temp file and the swap.

## 4. Expose it to the page

```cpp
boson::ipc::StoreController<Settings> store_controller(settings, {.read_only_keys = {"browser_args"}});
boson::ipc::Router<..., boson::ipc::StoreController<Settings>> router(..., store_controller);
boson::ipc::Bridge bridge(runner, marshal, router);
bridge.attach(webview, {.allowed_origins = {}, .capabilities = policy});
const auto settings_events = connect_store_events(store_controller, bridge); // after the bridge
```

| Method | Params | Result |
|---|---|---|
| `Store.get` | — | `T` |
| `Store.set` | `T` (missing keys take their defaults) | `StoreChange` |
| `Store.patch` | `Partial<T>` (unknown keys are an error; nested objects merge, arrays replace) | `StoreChange` |
| `Store.reset` | — | `StoreChange` (read-only keys keep their values) |
| `Store.info` | — | `StoreInfo` (name, path, load status, revision, dirty, restart / pending / read-only keys) |

* Permissions: the Bridge checks the WebView's `CapabilityPolicy` before the controller runs, so a settings page can
  get `"allow": ["Store.get", "Store.patch"]` and everything else `"deny": ["Store.reset"]` (rejected calls get
  -32003). `StoreControllerOptions::read_only_keys` adds a per-key guard: a set / patch that would change one fails
  with "'key' is read-only" and nothing is saved; C++ can still change them.
* Several stores: give each controller its own JS name, `StoreController<Prefs, "Prefs">`, and its own
  `StoreOptions::name` (reported as `store` in the event). `connect_store_events(controller, bridge, "prefs:changed")`
  takes the event name.
* `store:changed` carries `{store, value, change}` (`StoreChangedEvent<T>`) for changes from C++, from any WebView and
  from `reload()`; `change.sender_webview` is the `WebView::id()` of the IPC caller, so a page can skip its own echo,
  and `change.revision` orders events.
* tsgen: add `StoreController<T>` to the controller list and `StoreEvents<T>` to the event maps. The generated names are
  `T`, `StoreChange`, `StoreInfo` and `StoreChangedEvent_T` (TsGenerator now turns template names such as
  `StoreChangedEvent<ns::T>` into `StoreChangedEvent_T`, and a type with `using boson_ts_partial = U` into `Partial<U>`).

```ts
import type { DemoSettings, StoreChange } from './generated/boson-bindings';

const s = await boson.call<DemoSettings>('Store.get');
const r = await boson.call<StoreChange, Partial<DemoSettings>>('Store.patch', { tab_sleep_minutes: 30 });
if (r.restart_required) showRestartBanner(r.restart_keys);
boson.on('store:changed', (e) => applySettings(e.value));
```

JSON keys are snake_case like the rest of the Boson API (`restart_required`, not `restartRequired`).

The demo (`examples/demo`) keeps `DemoSettings` in `%LOCALAPPDATA%\Boson Demo\settings.json`, maps
`hardware_acceleration = false` to `--disable-gpu` and registers the controller and the event.

## Limits

* One process per file. Two processes writing the same file do not corrupt it (every save is atomic), but the
  last writer wins and neither sees the other's change until `reload()`. The file is not watched; call `reload()`
  yourself (for example from `ScopedFs::watch`).
* Opening deletes `<file>.*.tmp` files it can delete. A temp file that another process still has open cannot be
  deleted and is kept, but one that a writer has closed and not yet swapped in would be removed; do not open the
  same store from two processes at once.
* Change detection is per top-level key, on the JSON text. Equal `std::unordered_map` fields can serialise in a different
  order and then show up as changed.
* No schema versioning or migration framework: `normalize` plus "missing keys keep defaults, unknown keys are
  ignored" is the migration path.
* No encryption. For secrets use `SafeStorage` / `CredentialStore` (see [SECURITY.md](SECURITY.md)).

## Verification

Verified in `boson_store_tests` (`tests/test_store.cpp`, 194 checks, Debug, MSVC 14.52, Windows 11, WebView2 runtime
installed):

* Missing file: defaults, no file until the first change, parent folders created, `create_if_missing`, in-memory store.
* Corrupt file: invalid JSON, empty / whitespace, invalid UTF-8, wrong types, a JSON array and a file over
  `max_file_bytes` all load as `Corrupt` with defaults; the backup holds the original bytes; backups are capped;
  `max_corrupt_backups = 0` deletes; a good `.bak` is recovered.
* Unknown keys ignored (and rejected with `ignore_unknown_keys = false`), missing keys default, UTF-8 BOM, ADL and explicit
  `normalize`, restart keys from `T` and from options (unknown ones become warnings).
* Change detection (no write when nothing changed, a change `normalize` undoes is no change), cancel, `validate` veto,
  patch merge / array replace / unknown key / non-object rejection, set, reset, `autosave = false` + `save()`,
  `reload()` after an external edit, listener order and revisions, re-entrancy from an update function and from a
  listener (1131), throwing listener.
* Concurrent update: 8 threads x 100 increments with 2 reader threads and a listener: no lost update, readers never
  see the value go back, listener sees revisions 1..800 in order, the file holds 800.
* Crash during save: a child process (the test exe with `--crash-save`) terminates itself from `before_replace`; the
  test checks the exit code, exactly one temp file with the new content, the original byte-for-byte intact, and that
  the next open loads the original and removes the temp file.
* `write_file_atomic` (create, replace with `.bak`, without `.bak`, empty path), `default_store_path` validation,
  `update_on` / `save_on` run on a pool thread via `stdexec::sync_wait`.
* Controller through `Router::dispatch_sync`: get / set / patch / reset / info, sender id, read-only keys (set, patch,
  reset), a second controller named `Prefs`, events posted through `Bridge` while the subscription lives and not after.
  TypeScript output for `Store<T>` (`Partial<T>`, `StoreChangedEvent_T`).
* Real WebView2: `Store.patch` allowed by the `CapabilityPolicy`, `Store.set` (not granted) and `Store.reset` (denied) rejected with
  -32003, and `store:changed` from an IPC change and from a C++ change received by the page. This part skips without a runtime.

Not verified: a real power loss (only process termination was tested; durability relies on `FlushFileBuffers` +
`ReplaceFileW`), the ReplaceFileW 1176 / 1177 fallbacks and the sharing-violation retry (not provoked), `Unreadable`
(access-denied files), network / redirected AppData folders, and the demo's settings UI (the demo wires the store
and the controller but its page does not use them yet; the demo build was checked, not run).
