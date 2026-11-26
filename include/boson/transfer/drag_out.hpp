#pragma once

#include <windows.h>
#include <oleidl.h>
#include <shlobj.h>
#include <wrl.h>

#include <boson/exec/ui_marshal.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::transfer {

enum class DragEffect : std::uint8_t {
    None, ///< cancelled (Esc) or dropped where nothing was accepted
    Copy,
    Move,
    Link,
};

[[nodiscard]] std::string_view to_string(DragEffect effect) noexcept;

struct DragOutOptions {
    /// Absolute paths of existing files or folders.
    std::vector<std::filesystem::path> files;
    /// Drag image: any WIC-decodable file (PNG, ICO, BMP, JPEG), scaled to at most 256 px. Empty = the image the
    /// shell builds for the items (as in Explorer).
    std::filesystem::path icon;
    /// DROPEFFECT_* the drop target may choose from. Move lets e.g. Explorer move the file away.
    DWORD allowed_effects = DROPEFFECT_COPY | DROPEFFECT_LINK;
    /// Refuse to start when no mouse button is down: a drag started without a pressed button would drop
    /// immediately onto whatever is under the cursor.
    bool require_button_down = true;
};

struct DragOutResult {
    /// The modal drag loop ran (false: rejected before DoDragDrop; see error).
    bool started = false;
    DragEffect effect = DragEffect::None;
    HRESULT hr = S_OK;
    std::string error;
};

/// @brief Native drag-out of files (Electron's webContents.startDrag): a shell IDataObject (Shell IDList Array,
/// CF_HDROP, Preferred DropEffect, optional drag image) handed to SHDoDragDrop.
///
/// DoDragDrop runs a modal message loop on the UI thread until the button is released. The loop keeps pumping
/// messages (painting, WebView2, UiMarshal tasks, timers keep working), but code after the call - and the
/// application's own message loop, e.g. accelerator translation - only resumes when the drag ends. start()
/// therefore never runs the loop inside the caller (an IPC handler or a WebView2 event): it posts the drag to
/// the UI thread through UiMarshal and reports the outcome to a callback.
class DragOut {
public:
    using Callback = std::function<void(const DragOutResult& result)>;

    /// @param owner window passed to SHDoDragDrop (drag-image owner); usually the main window.
    DragOut(exec::UiMarshal& marshal, HWND owner) noexcept;
    ~DragOut();

    DragOut(const DragOut&) = delete;
    DragOut& operator=(const DragOut&) = delete;
    DragOut(DragOut&&) = delete;
    DragOut& operator=(DragOut&&) = delete;

    /// @brief Any thread. Queues the drag on the UI thread; done runs on the UI thread when it ends (or is
    /// rejected: another drag in progress, no button down, invalid files). false when it could not be queued.
    bool start(DragOutOptions options, Callback done);
    [[nodiscard]] bool is_dragging() const noexcept;
    void set_owner(HWND owner) noexcept { m_owner.store(owner, std::memory_order_release); }

    /// @brief UI thread: validates, builds the data object and runs the modal drag loop (blocks until drop/cancel).
    [[nodiscard]] static DragOutResult run(HWND owner, const DragOutOptions& options);

    /// @brief Shell data object for files: Shell IDList Array (+ what the shell adds), CF_HDROP and
    /// Preferred DropEffect = copy. Files in different folders are supported.
    [[nodiscard]] static HRESULT create_data_object(
        std::span<const std::filesystem::path> files, Microsoft::WRL::ComPtr<IDataObject>& out) noexcept;
    /// @brief Decodes image (WIC) and attaches it with IDragSourceHelper::InitializeFromBitmap. offset is the
    /// cursor position inside the image; {-1,-1} = centre.
    [[nodiscard]] static HRESULT set_drag_image(IDataObject* data, const std::filesystem::path& image, POINT offset = {-1, -1}) noexcept;

    /// @brief Paths stored as CF_HDROP in data (empty when absent).
    [[nodiscard]] static std::vector<std::wstring> hdrop_paths(IDataObject* data);
    /// @brief QueryGetData for a standard (CF_*) or registered format name such as CFSTR_SHELLIDLIST.
    [[nodiscard]] static bool has_format(IDataObject* data, CLIPFORMAT format) noexcept;
    [[nodiscard]] static bool has_format(IDataObject* data, const wchar_t* registered_name) noexcept;
    /// @brief DWORD stored under a registered format (e.g. CFSTR_PREFERREDDROPEFFECT).
    [[nodiscard]] static std::optional<DWORD> dword_format(IDataObject* data, const wchar_t* registered_name) noexcept;
    [[nodiscard]] static bool is_mouse_button_down() noexcept;
    /// @brief DROPEFFECT_* from names ("copy", "move", "link"); 0 when a name is unknown.
    [[nodiscard]] static DWORD parse_effects(std::span<const std::string> names) noexcept;

private:
    struct State {
        std::atomic<bool> alive{true};
        std::atomic<bool> dragging{false};
    };

    exec::UiMarshal& m_marshal;
    std::atomic<HWND> m_owner;
    std::shared_ptr<State> m_state = std::make_shared<State>();
};

} // namespace boson::transfer
