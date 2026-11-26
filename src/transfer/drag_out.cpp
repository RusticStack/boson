#include <boson/transfer/drag_out.hpp>

#include <boson/core/result.hpp>

#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

namespace boson::transfer {

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kMaxDragImagePx = 256;

/// Absolute IDLists owned for the duration of create_data_object (std::vector cannot hold __unaligned pointers
/// without C4090, so plain arrays from make_unique are used).
class PidlArray {
public:
    explicit PidlArray(std::size_t count)
        : m_owned(std::make_unique<PIDLIST_ABSOLUTE[]>(count)),
          m_view(std::make_unique<PCIDLIST_ABSOLUTE[]>(count)),
          m_count(count) {}
    ~PidlArray() {
        for (std::size_t i = 0; i < m_count; ++i) {
            ::CoTaskMemFree(m_owned[i]);
        }
    }
    PidlArray(const PidlArray&) = delete;
    PidlArray& operator=(const PidlArray&) = delete;

    void set(std::size_t index, PIDLIST_ABSOLUTE pidl) noexcept {
        m_owned[index] = pidl;
        m_view[index] = pidl;
    }
    [[nodiscard]] PCIDLIST_ABSOLUTE* data() const noexcept { return m_view.get(); }

private:
    std::unique_ptr<PIDLIST_ABSOLUTE[]> m_owned;
    std::unique_ptr<PCIDLIST_ABSOLUTE[]> m_view;
    std::size_t m_count;
};

struct GlobalDeleter {
    void operator()(void* memory) const noexcept { ::GlobalFree(memory); }
};
using UniqueGlobal = std::unique_ptr<void, GlobalDeleter>;

struct BitmapDeleter {
    void operator()(HBITMAP__* bitmap) const noexcept { ::DeleteObject(bitmap); }
};
using UniqueBitmap = std::unique_ptr<HBITMAP__, BitmapDeleter>;

/// Balanced OleInitialize for the drag (DoDragDrop requires OLE, not just COM, on this thread).
class OleScope {
public:
    OleScope() noexcept : m_hr(::OleInitialize(nullptr)) {}
    ~OleScope() {
        if (SUCCEEDED(m_hr)) {
            ::OleUninitialize();
        }
    }
    OleScope(const OleScope&) = delete;
    OleScope& operator=(const OleScope&) = delete;
    [[nodiscard]] HRESULT hr() const noexcept { return m_hr; }

private:
    HRESULT m_hr;
};

class DropSource final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropSource> {
public:
    STDMETHODIMP QueryContinueDrag(BOOL escape_pressed, DWORD key_state) override {
        if (escape_pressed != FALSE) {
            return DRAGDROP_S_CANCEL;
        }
        if ((key_state & (MK_LBUTTON | MK_RBUTTON)) == 0) {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    STDMETHODIMP GiveFeedback(DWORD /*effect*/) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

FORMATETC format_of(CLIPFORMAT format, DWORD tymed = TYMED_HGLOBAL) noexcept {
    return FORMATETC{format, nullptr, DVASPECT_CONTENT, -1, tymed};
}

HRESULT set_global(IDataObject* data, CLIPFORMAT format, const void* bytes, std::size_t size) noexcept {
    UniqueGlobal memory{::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size)};
    if (!memory) {
        return E_OUTOFMEMORY;
    }
    void* locked = ::GlobalLock(memory.get());
    if (locked == nullptr) {
        return E_OUTOFMEMORY;
    }
    std::memcpy(locked, bytes, size);
    ::GlobalUnlock(memory.get());
    FORMATETC fe = format_of(format);
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = memory.get();
    const HRESULT hr = data->SetData(&fe, &medium, TRUE);
    if (SUCCEEDED(hr)) {
        static_cast<void>(memory.release()); // owned by the data object now
    }
    return hr;
}

/// DROPFILES + double-NUL-terminated UTF-16 list, for data objects that do not synthesize CF_HDROP themselves.
HRESULT ensure_hdrop(IDataObject* data, std::span<const std::filesystem::path> files) noexcept {
    FORMATETC fe = format_of(CF_HDROP);
    if (data->QueryGetData(&fe) == S_OK) {
        return S_OK;
    }
    try {
        std::wstring list;
        for (const auto& file : files) {
            list.append(file.wstring());
            list.push_back(L'\0');
        }
        list.push_back(L'\0');
        std::vector<std::byte> buffer(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
        DROPFILES header{};
        header.pFiles = sizeof(DROPFILES);
        header.fWide = TRUE;
        std::memcpy(buffer.data(), &header, sizeof(header));
        std::memcpy(buffer.data() + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
        return set_global(data, CF_HDROP, buffer.data(), buffer.size());
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

DragEffect effect_from(DWORD effect) noexcept {
    if ((effect & DROPEFFECT_MOVE) != 0) {
        return DragEffect::Move;
    }
    if ((effect & DROPEFFECT_COPY) != 0) {
        return DragEffect::Copy;
    }
    if ((effect & DROPEFFECT_LINK) != 0) {
        return DragEffect::Link;
    }
    return DragEffect::None;
}

DragOutResult rejected(HRESULT hr, std::string error) {
    return DragOutResult{.started = false, .effect = DragEffect::None, .hr = hr, .error = std::move(error)};
}

} // namespace

std::string_view to_string(DragEffect effect) noexcept {
    switch (effect) {
    case DragEffect::Copy:
        return "copy";
    case DragEffect::Move:
        return "move";
    case DragEffect::Link:
        return "link";
    case DragEffect::None:
        break;
    }
    return "none";
}

DragOut::DragOut(exec::UiMarshal& marshal, HWND owner) noexcept
    : m_marshal(marshal),
      m_owner(owner) {}

DragOut::~DragOut() {
    m_state->alive.store(false, std::memory_order_release);
}

bool DragOut::is_dragging() const noexcept {
    return m_state->dragging.load(std::memory_order_acquire);
}

bool DragOut::is_mouse_button_down() noexcept {
    const auto down = [](int key) { return (::GetAsyncKeyState(key) & 0x8000) != 0; };
    const bool swapped = ::GetSystemMetrics(SM_SWAPBUTTON) != 0;
    return down(swapped ? VK_RBUTTON : VK_LBUTTON) || down(swapped ? VK_LBUTTON : VK_RBUTTON);
}

DWORD DragOut::parse_effects(std::span<const std::string> names) noexcept {
    DWORD effects = 0;
    for (const auto& name : names) {
        if (name == "copy") {
            effects |= DROPEFFECT_COPY;
        } else if (name == "move") {
            effects |= DROPEFFECT_MOVE;
        } else if (name == "link") {
            effects |= DROPEFFECT_LINK;
        } else {
            return 0;
        }
    }
    return effects;
}

bool DragOut::start(DragOutOptions options, Callback done) {
    try {
        m_marshal.post_task([state = m_state, owner = m_owner.load(std::memory_order_acquire), options = std::move(options),
                             done = std::move(done)]() {
            if (!state->alive.load(std::memory_order_acquire)) {
                return;
            }
            DragOutResult result;
            if (state->dragging.exchange(true, std::memory_order_acq_rel)) {
                result = rejected(E_UNEXPECTED, "a drag is already in progress");
            } else {
                result = run(owner, options);
                state->dragging.store(false, std::memory_order_release);
            }
            if (done && state->alive.load(std::memory_order_acquire)) {
                done(result);
            }
        });
        return true;
    } catch (...) {
        return false;
    }
}

DragOutResult DragOut::run(HWND owner, const DragOutOptions& options) {
    if (options.files.empty()) {
        return rejected(E_INVALIDARG, "no files to drag");
    }
    for (const auto& file : options.files) {
        if (!file.is_absolute() || ::GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return rejected(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "not an existing absolute path: " + core::wide_to_utf8(file.wstring()));
        }
    }
    if ((options.allowed_effects & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK)) == 0) {
        return rejected(E_INVALIDARG, "no drop effect allowed");
    }
    if (options.require_button_down && !is_mouse_button_down()) {
        return rejected(S_FALSE, "no mouse button is down (start the drag from mousedown / dragstart)");
    }

    const OleScope ole;
    if (FAILED(ole.hr())) {
        core::hr_check(ole.hr(), "DragOut: OleInitialize");
        return rejected(ole.hr(), "OleInitialize failed (the UI thread must be a single-threaded apartment)");
    }
    ComPtr<IDataObject> data;
    if (const HRESULT hr = create_data_object(options.files, data); FAILED(hr)) {
        core::hr_check(hr, "DragOut: create_data_object");
        return rejected(hr, "could not create the shell data object");
    }
    if (!options.icon.empty()) {
        core::hr_check(set_drag_image(data.Get(), options.icon), "DragOut: set_drag_image (drag continues with the default image)");
    }

    const ComPtr<IDropSource> source = Microsoft::WRL::Make<DropSource>();
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = ::SHDoDragDrop(owner, data.Get(), source.Get(), options.allowed_effects, &effect);
    if (hr == DRAGDROP_S_DROP) {
        return DragOutResult{.started = true, .effect = effect_from(effect), .hr = hr, .error = {}};
    }
    if (hr == DRAGDROP_S_CANCEL) {
        return DragOutResult{.started = true, .effect = DragEffect::None, .hr = hr, .error = {}};
    }
    core::hr_check(hr, "DragOut: SHDoDragDrop");
    return DragOutResult{.started = true, .effect = DragEffect::None, .hr = hr, .error = "SHDoDragDrop failed"};
}

HRESULT DragOut::create_data_object(std::span<const std::filesystem::path> files, ComPtr<IDataObject>& out) noexcept {
    out.Reset();
    if (files.empty()) {
        return E_INVALIDARG;
    }
    try {
        PidlArray pidls(files.size());
        for (std::size_t i = 0; i < files.size(); ++i) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            const HRESULT hr = ::SHParseDisplayName(files[i].c_str(), nullptr, &pidl, 0, nullptr);
            if (FAILED(hr) || pidl == nullptr) {
                return FAILED(hr) ? hr : E_FAIL;
            }
            pidls.set(i, pidl);
        }
        const UINT count = static_cast<UINT>(files.size());

        ComPtr<IShellItemArray> items;
        HRESULT hr = ::SHCreateShellItemArrayFromIDLists(count, pidls.data(), &items);
        if (SUCCEEDED(hr)) {
            hr = items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&out));
        }
        if (FAILED(hr) || !out) {
            // Items without a common parent: absolute IDLists relative to the desktop.
            hr = ::SHCreateDataObject(nullptr, count, reinterpret_cast<PCUITEMID_CHILD_ARRAY>(pidls.data()), nullptr, IID_PPV_ARGS(&out));
        }
        if (FAILED(hr) || !out) {
            out.Reset();
            return FAILED(hr) ? hr : E_FAIL;
        }
        if (hr = ensure_hdrop(out.Get(), files); FAILED(hr)) {
            out.Reset();
            return hr;
        }
        const CLIPFORMAT preferred = static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        const DWORD copy = DROPEFFECT_COPY;
        core::hr_check(set_global(out.Get(), preferred, &copy, sizeof(copy)), "DragOut: Preferred DropEffect");
        return S_OK;
    } catch (...) {
        out.Reset();
        return E_OUTOFMEMORY;
    }
}

HRESULT DragOut::set_drag_image(IDataObject* data, const std::filesystem::path& image, POINT offset) noexcept {
    if (data == nullptr || image.empty()) {
        return E_INVALIDARG;
    }
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (hr = factory->CreateDecoderFromFilename(image.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder); FAILED(hr)) {
        return hr;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (hr = decoder->GetFrame(0, &frame); FAILED(hr)) {
        return hr;
    }
    UINT width = 0;
    UINT height = 0;
    if (hr = frame->GetSize(&width, &height); FAILED(hr) || width == 0 || height == 0) {
        return FAILED(hr) ? hr : E_FAIL;
    }
    ComPtr<IWICBitmapSource> source = frame;
    if (width > kMaxDragImagePx || height > kMaxDragImagePx) {
        const double scale = static_cast<double>(kMaxDragImagePx) / static_cast<double>(std::max(width, height));
        width = std::max(1U, static_cast<UINT>(width * scale));
        height = std::max(1U, static_cast<UINT>(height * scale));
        ComPtr<IWICBitmapScaler> scaler;
        if (hr = factory->CreateBitmapScaler(&scaler); FAILED(hr)) {
            return hr;
        }
        if (hr = scaler->Initialize(source.Get(), width, height, WICBitmapInterpolationModeFant); FAILED(hr)) {
            return hr;
        }
        source = scaler;
    }
    ComPtr<IWICFormatConverter> converter;
    if (hr = factory->CreateFormatConverter(&converter); FAILED(hr)) {
        return hr;
    }
    if (hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        FAILED(hr)) {
        return hr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    UniqueBitmap bitmap{::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0)};
    if (!bitmap || bits == nullptr) {
        return E_OUTOFMEMORY;
    }
    const UINT stride = width * 4;
    if (hr = converter->CopyPixels(nullptr, stride, stride * height, static_cast<BYTE*>(bits)); FAILED(hr)) {
        return hr;
    }

    ComPtr<IDragSourceHelper> helper;
    if (hr = ::CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)); FAILED(hr)) {
        return hr;
    }
    SHDRAGIMAGE drag_image{};
    drag_image.sizeDragImage = SIZE{static_cast<LONG>(width), static_cast<LONG>(height)};
    drag_image.ptOffset = offset.x < 0 || offset.y < 0 ? POINT{static_cast<LONG>(width / 2), static_cast<LONG>(height / 2)} : offset;
    drag_image.hbmpDragImage = bitmap.get();
    drag_image.crColorKey = CLR_NONE;
    hr = helper->InitializeFromBitmap(&drag_image, data);
    if (SUCCEEDED(hr)) {
        static_cast<void>(bitmap.release()); // the drag-image manager owns it now
    }
    return hr;
}

std::vector<std::wstring> DragOut::hdrop_paths(IDataObject* data) {
    std::vector<std::wstring> out;
    if (data == nullptr) {
        return out;
    }
    FORMATETC fe = format_of(CF_HDROP);
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&fe, &medium))) {
        return out;
    }
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal != nullptr) {
        if (auto* drop = static_cast<HDROP>(::GlobalLock(medium.hGlobal)); drop != nullptr) {
            const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
                const UINT copied = ::DragQueryFileW(drop, i, path.data(), length + 1);
                path.resize(copied);
                out.push_back(std::move(path));
            }
            ::GlobalUnlock(medium.hGlobal);
        }
    }
    ::ReleaseStgMedium(&medium);
    return out;
}

bool DragOut::has_format(IDataObject* data, CLIPFORMAT format) noexcept {
    if (data == nullptr) {
        return false;
    }
    FORMATETC fe = format_of(format, TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ISTORAGE);
    return data->QueryGetData(&fe) == S_OK;
}

bool DragOut::has_format(IDataObject* data, const wchar_t* registered_name) noexcept {
    return has_format(data, static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(registered_name)));
}

std::optional<DWORD> DragOut::dword_format(IDataObject* data, const wchar_t* registered_name) noexcept {
    if (data == nullptr) {
        return std::nullopt;
    }
    FORMATETC fe = format_of(static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(registered_name)));
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&fe, &medium))) {
        return std::nullopt;
    }
    std::optional<DWORD> value;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal != nullptr && ::GlobalSize(medium.hGlobal) >= sizeof(DWORD)) {
        if (const auto* locked = static_cast<const DWORD*>(::GlobalLock(medium.hGlobal)); locked != nullptr) {
            value = *locked;
            ::GlobalUnlock(medium.hGlobal);
        }
    }
    ::ReleaseStgMedium(&medium);
    return value;
}

} // namespace boson::transfer
