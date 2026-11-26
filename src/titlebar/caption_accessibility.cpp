#include <boson/titlebar/caption_accessibility.hpp>

#include <boson/core/result.hpp>

#include <UIAutomation.h>
#include <oleauto.h>

#include <algorithm>
#include <array>
#include <unordered_map>

namespace boson::titlebar {

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace {

const wchar_t* automation_id(CaptionButton kind) noexcept {
    switch (kind) {
    case CaptionButton::Minimize: return L"Minimize";
    case CaptionButton::Maximize: return L"Maximize";
    case CaptionButton::Close: return L"Close";
    }
    return L"";
}

HRESULT bstr_variant(VARIANT* out, const std::wstring& text) noexcept {
    out->vt = VT_BSTR;
    out->bstrVal = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
    return out->bstrVal != nullptr ? S_OK : E_OUTOFMEMORY;
}

void bool_variant(VARIANT* out, bool value) noexcept {
    out->vt = VT_BOOL;
    out->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
}

void int_variant(VARIANT* out, int value) noexcept {
    out->vt = VT_I4;
    out->lVal = value;
}

class RootProvider;

} // namespace

struct CaptionAccessibility::State {
    HWND host = nullptr;
    CaptionAccessibilitySource source;
    bool alive = true;
    ComPtr<RootProvider> root;
    std::array<ComPtr<IRawElementProviderSimple>, kCaptionButtonCount> buttons{};

    [[nodiscard]] std::vector<AccessibleButton> list() const {
        if (!alive || !source.buttons) {
            return {};
        }
        return source.buttons();
    }
};

namespace {

using State = CaptionAccessibility::State;

class ButtonProvider final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IRawElementProviderSimple, IRawElementProviderFragment, IInvokeProvider> {
public:
    ButtonProvider(std::weak_ptr<State> state, CaptionButton kind) noexcept
        : m_state(std::move(state))
        , m_kind(kind) {}

    // IRawElementProviderSimple
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }

    IFACEMETHODIMP GetPatternProvider(PATTERNID pattern, IUnknown** out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = nullptr;
        if (!find().has_value()) return UIA_E_ELEMENTNOTAVAILABLE;
        if (pattern == UIA_InvokePatternId) {
            return QueryInterface(IID_PPV_ARGS(reinterpret_cast<IInvokeProvider**>(out)));
        }
        return S_OK;
    }

    IFACEMETHODIMP GetPropertyValue(PROPERTYID property, VARIANT* out) override {
        if (out == nullptr) return E_INVALIDARG;
        VariantInit(out);
        const auto button = find();
        if (!button.has_value()) return UIA_E_ELEMENTNOTAVAILABLE;
        switch (property) {
        case UIA_ControlTypePropertyId: int_variant(out, UIA_ButtonControlTypeId); break;
        case UIA_NamePropertyId: return bstr_variant(out, button->name);
        case UIA_AutomationIdPropertyId: return bstr_variant(out, automation_id(m_kind));
        case UIA_FrameworkIdPropertyId: return bstr_variant(out, L"Win32");
        case UIA_ClassNamePropertyId: return bstr_variant(out, L"BosonCaptionButton");
        case UIA_IsEnabledPropertyId: bool_variant(out, button->enabled); break;
        case UIA_IsKeyboardFocusablePropertyId: bool_variant(out, false); break;
        case UIA_HasKeyboardFocusPropertyId: bool_variant(out, false); break;
        case UIA_IsInvokePatternAvailablePropertyId: bool_variant(out, true); break;
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId: bool_variant(out, true); break;
        case UIA_IsOffscreenPropertyId: bool_variant(out, IsRectEmpty(&button->screen_rect) != FALSE); break;
        default: break;
        }
        return S_OK;
    }

    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = nullptr;
        return S_OK;
    }

    // IRawElementProviderFragment
    IFACEMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override;

    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = nullptr;
        if (!find().has_value()) return UIA_E_ELEMENTNOTAVAILABLE;
        SAFEARRAY* array = SafeArrayCreateVector(VT_I4, 0, 2);
        if (array == nullptr) return E_OUTOFMEMORY;
        std::array<LONG, 2> ids{UiaAppendRuntimeId, static_cast<LONG>(m_kind) + 1};
        for (LONG i = 0; i < 2; ++i) {
            SafeArrayPutElement(array, &i, &ids[static_cast<std::size_t>(i)]);
        }
        *out = array;
        return S_OK;
    }

    IFACEMETHODIMP get_BoundingRectangle(UiaRect* out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = UiaRect{};
        const auto button = find();
        if (!button.has_value()) return UIA_E_ELEMENTNOTAVAILABLE;
        const RECT& rc = button->screen_rect;
        *out = UiaRect{
            static_cast<double>(rc.left),
            static_cast<double>(rc.top),
            static_cast<double>(rc.right - rc.left),
            static_cast<double>(rc.bottom - rc.top),
        };
        return S_OK;
    }

    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = nullptr;
        return S_OK;
    }

    IFACEMETHODIMP SetFocus() override { return S_OK; }

    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** out) override;

    // IInvokeProvider
    IFACEMETHODIMP Invoke() override {
        const auto state = m_state.lock();
        const auto button = find();
        if (!state || !button.has_value()) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!button->enabled) return UIA_E_ELEMENTNOTENABLED;
        if (state->source.invoke) {
            state->source.invoke(m_kind);
        }
        return S_OK;
    }

    [[nodiscard]] std::optional<AccessibleButton> find() const {
        const auto state = m_state.lock();
        if (!state) return std::nullopt;
        for (auto& b : state->list()) {
            if (b.kind == m_kind) return b;
        }
        return std::nullopt;
    }

private:
    std::weak_ptr<State> m_state;
    CaptionButton m_kind;
};

class RootProvider final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IRawElementProviderSimple, IRawElementProviderFragment, IRawElementProviderFragmentRoot> {
public:
    explicit RootProvider(std::weak_ptr<State> state) noexcept
        : m_state(std::move(state)) {}

    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }

    IFACEMETHODIMP GetPatternProvider(PATTERNID /*pattern*/, IUnknown** out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = nullptr;
        return S_OK;
    }

    IFACEMETHODIMP GetPropertyValue(PROPERTYID property, VARIANT* out) override {
        if (out == nullptr) return E_INVALIDARG;
        VariantInit(out);
        const auto state = m_state.lock();
        if (!state || !state->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        switch (property) {
        case UIA_ControlTypePropertyId: int_variant(out, UIA_TitleBarControlTypeId); break;
        case UIA_NamePropertyId:
            if (state->source.container_name) {
                return bstr_variant(out, state->source.container_name());
            }
            break;
        case UIA_AutomationIdPropertyId: return bstr_variant(out, L"BosonCaptionButtons");
        case UIA_IsKeyboardFocusablePropertyId: bool_variant(out, false); break;
        case UIA_IsControlElementPropertyId: bool_variant(out, true); break;
        case UIA_IsContentElementPropertyId: bool_variant(out, false); break;
        default: break;
        }
        return S_OK;
    }

    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = nullptr;
        const auto state = m_state.lock();
        if (!state || !state->alive || state->host == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;
        return UiaHostProviderFromHwnd(state->host, out);
    }

    IFACEMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = nullptr;
        const auto state = m_state.lock();
        if (!state || !state->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        if (direction != NavigateDirection_FirstChild && direction != NavigateDirection_LastChild) {
            return S_OK; // parent / siblings of the root come from the HWND host provider
        }
        const auto list = state->list();
        if (list.empty()) return S_OK;
        const CaptionButton kind = direction == NavigateDirection_FirstChild ? list.front().kind : list.back().kind;
        return button_fragment(*state, kind, out);
    }

    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = nullptr; // the HWND host supplies the runtime id
        return S_OK;
    }

    IFACEMETHODIMP get_BoundingRectangle(UiaRect* out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = UiaRect{}; // the HWND host supplies the bounds
        return S_OK;
    }

    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = nullptr;
        return S_OK;
    }

    IFACEMETHODIMP SetFocus() override { return S_OK; }

    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
        if (out == nullptr) return E_INVALIDARG;
        return QueryInterface(IID_PPV_ARGS(out));
    }

    IFACEMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = nullptr;
        const auto state = m_state.lock();
        if (!state || !state->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        const POINT pt{static_cast<LONG>(x), static_cast<LONG>(y)};
        for (const auto& b : state->list()) {
            if (PtInRect(&b.screen_rect, pt) != FALSE) {
                return button_fragment(*state, b.kind, out);
            }
        }
        return S_OK;
    }

    IFACEMETHODIMP GetFocus(IRawElementProviderFragment** out) override {
        if (out == nullptr) return E_INVALIDARG;
        *out = nullptr;
        return S_OK;
    }

    static HRESULT button_fragment(State& state, CaptionButton kind, IRawElementProviderFragment** out) {
        auto& slot = state.buttons[static_cast<std::size_t>(kind)];
        if (!slot) {
            const std::shared_ptr<State> self = state.root ? state.root->m_state.lock() : nullptr;
            ComPtr<ButtonProvider> button = Make<ButtonProvider>(self, kind);
            if (!button) return E_OUTOFMEMORY;
            slot = button;
        }
        return slot.CopyTo(IID_PPV_ARGS(out));
    }

    std::weak_ptr<State> m_state;
};

IFACEMETHODIMP ButtonProvider::Navigate(NavigateDirection direction, IRawElementProviderFragment** out) {
    if (out == nullptr) return E_INVALIDARG;
    *out = nullptr;
    const auto state = m_state.lock();
    if (!state || !state->alive || !state->root) return UIA_E_ELEMENTNOTAVAILABLE;
    if (direction == NavigateDirection_Parent) {
        return state->root.CopyTo(IID_PPV_ARGS(out));
    }
    if (direction != NavigateDirection_NextSibling && direction != NavigateDirection_PreviousSibling) {
        return S_OK;
    }
    const auto list = state->list();
    const auto it = std::find_if(list.begin(), list.end(), [this](const AccessibleButton& b) { return b.kind == m_kind; });
    if (it == list.end()) return UIA_E_ELEMENTNOTAVAILABLE;
    if (direction == NavigateDirection_NextSibling) {
        if (it + 1 == list.end()) return S_OK;
        return RootProvider::button_fragment(*state, (it + 1)->kind, out);
    }
    if (it == list.begin()) return S_OK;
    return RootProvider::button_fragment(*state, (it - 1)->kind, out);
}

IFACEMETHODIMP ButtonProvider::get_FragmentRoot(IRawElementProviderFragmentRoot** out) {
    if (out == nullptr) return E_INVALIDARG;
    *out = nullptr;
    const auto state = m_state.lock();
    if (!state || !state->alive || !state->root) return UIA_E_ELEMENTNOTAVAILABLE;
    return state->root.CopyTo(IID_PPV_ARGS(out));
}

} // namespace

CaptionAccessibility::CaptionAccessibility(HWND host, CaptionAccessibilitySource source)
    : m_state(std::make_shared<State>()) {
    m_state->host = host;
    m_state->source = std::move(source);
    m_state->root = Make<RootProvider>(m_state);
}

CaptionAccessibility::~CaptionAccessibility() {
    disconnect();
}

void CaptionAccessibility::disconnect() noexcept {
    if (!m_state || !m_state->alive) {
        return;
    }
    m_state->alive = false;
    for (auto& button : m_state->buttons) {
        if (button) {
            UiaDisconnectProvider(button.Get());
            button.Reset();
        }
    }
    if (m_state->root) {
        UiaDisconnectProvider(m_state->root.Get());
    }
    if (m_state->host != nullptr && IsWindow(m_state->host)) {
        // Tells UIA the HWND no longer serves providers (documented teardown step).
        UiaReturnRawElementProvider(m_state->host, 0, 0, nullptr);
    }
    m_state->root.Reset();
}

std::optional<LRESULT> CaptionAccessibility::handle_get_object(WPARAM w_param, LPARAM l_param) {
    if (!m_state || !m_state->alive || !m_state->root) {
        return std::nullopt;
    }
    if (static_cast<LONG>(l_param) != static_cast<LONG>(UiaRootObjectId)) {
        return std::nullopt;
    }
    return UiaReturnRawElementProvider(m_state->host, w_param, l_param, m_state->root.Get());
}

void CaptionAccessibility::notify_name_changed(CaptionButton kind, const std::wstring& old_name, const std::wstring& new_name) {
    if (!m_state || !m_state->alive || old_name == new_name || UiaClientsAreListening() == FALSE) {
        return;
    }
    ComPtr<IRawElementProviderFragment> fragment;
    if (FAILED(RootProvider::button_fragment(*m_state, kind, &fragment)) || !fragment) {
        return;
    }
    ComPtr<IRawElementProviderSimple> simple;
    if (FAILED(fragment.As(&simple))) {
        return;
    }
    VARIANT old_value{};
    VARIANT new_value{};
    VariantInit(&old_value);
    VariantInit(&new_value);
    if (SUCCEEDED(bstr_variant(&old_value, old_name)) && SUCCEEDED(bstr_variant(&new_value, new_name))) {
        BOSON_HR_CHECK(UiaRaiseAutomationPropertyChangedEvent(simple.Get(), UIA_NamePropertyId, old_value, new_value));
    }
    VariantClear(&old_value);
    VariantClear(&new_value);
}

// ---- HTML-button maximize overlay ----------------------------------------------------------------------

namespace {

std::unordered_map<HWND, std::unique_ptr<CaptionAccessibility>>& html_overlays() {
    static std::unordered_map<HWND, std::unique_ptr<CaptionAccessibility>> map; // UI thread only
    return map;
}

} // namespace

std::optional<LRESULT> html_caption_overlay_get_object(HWND overlay, HWND parent, WPARAM w_param, LPARAM l_param) {
    if (static_cast<LONG>(l_param) != static_cast<LONG>(UiaRootObjectId)) {
        return std::nullopt;
    }
    auto& map = html_overlays();
    auto it = map.find(overlay);
    if (it == map.end()) {
        CaptionAccessibilitySource source{
            .buttons = [overlay, parent]() {
                std::vector<AccessibleButton> list;
                if (!IsWindow(overlay) || IsWindowVisible(overlay) == FALSE) {
                    return list;
                }
                RECT rc{};
                GetWindowRect(overlay, &rc);
                list.push_back(AccessibleButton{
                    .kind = CaptionButton::Maximize,
                    .screen_rect = rc,
                    .name = localized_caption_name(parent, CaptionButton::Maximize, IsZoomed(parent) != FALSE),
                    .enabled = true,
                });
                return list;
            },
            .invoke = [parent](CaptionButton) {
                PostMessageW(parent, WM_SYSCOMMAND, IsZoomed(parent) != FALSE ? SC_RESTORE : SC_MAXIMIZE, 0);
            },
            .container_name = [parent]() {
                std::array<wchar_t, 256> title{};
                const int len = GetWindowTextW(parent, title.data(), static_cast<int>(title.size()));
                return std::wstring(title.data(), static_cast<std::size_t>(std::max(len, 0)));
            },
        };
        it = map.emplace(overlay, std::make_unique<CaptionAccessibility>(overlay, std::move(source))).first;
    }
    return it->second->handle_get_object(w_param, l_param);
}

void html_caption_overlay_destroyed(HWND overlay) noexcept {
    auto& map = html_overlays();
    if (const auto it = map.find(overlay); it != map.end()) {
        it->second->disconnect();
        map.erase(it);
    }
}

} // namespace boson::titlebar
