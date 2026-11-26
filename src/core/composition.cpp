#include <boson/core/composition.hpp>

#include <boson/core/result.hpp>

#include <algorithm>

namespace boson::core {

using Microsoft::WRL::ComPtr;

std::shared_ptr<CompositionHost> CompositionHost::create(HWND hwnd) {
    if (hwnd == nullptr) {
        return nullptr;
    }
    auto host = std::make_shared<CompositionHost>(PassKey{}, hwnd);
    if (!host->initialize()) {
        return nullptr;
    }
    return host;
}

CompositionHost::CompositionHost(PassKey, HWND hwnd)
    : m_hwnd(hwnd) {}

CompositionHost::~CompositionHost() {
    if (m_root) {
        m_root->RemoveAllVisuals();
    }
    if (m_target) {
        m_target->SetRoot(nullptr);
    }
    commit();
}

bool CompositionHost::initialize() {
    if (!BOSON_HR_CHECK(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&m_device))) || !m_device) {
        return false;
    }
    if (!BOSON_HR_CHECK(m_device->CreateTargetForHwnd(m_hwnd, TRUE, &m_target)) || !m_target) {
        return false;
    }
    if (!BOSON_HR_CHECK(m_device->CreateVisual(&m_root)) || !m_root) {
        return false;
    }
    if (!BOSON_HR_CHECK(m_device->CreateVisual(&m_underlay)) || !BOSON_HR_CHECK(m_device->CreateVisual(&m_overlay))) {
        return false;
    }
    if (!BOSON_HR_CHECK(m_target->SetRoot(m_root.Get()))) {
        return false;
    }
    rebuild_tree();
    return commit();
}

ComPtr<IDCompositionVisual2> CompositionHost::create_visual() const {
    ComPtr<IDCompositionVisual2> visual;
    if (m_device) {
        BOSON_HR_CHECK(m_device->CreateVisual(&visual));
    }
    return visual;
}

void CompositionHost::attach(WebView* owner, IDCompositionVisual2* visual) {
    if (owner == nullptr || visual == nullptr) {
        return;
    }
    detach(owner);
    m_entries.push_back(Entry{.owner = owner, .visual = visual});
    rebuild_tree();
}

void CompositionHost::detach(WebView* owner) noexcept {
    const auto it = std::ranges::find(m_entries, owner, &Entry::owner);
    if (it == m_entries.end()) {
        return;
    }
    if (m_root) {
        m_root->RemoveVisual(it->visual.Get());
    }
    m_entries.erase(it);
    commit();
}

void CompositionHost::raise(WebView* owner) {
    const auto it = std::ranges::find(m_entries, owner, &Entry::owner);
    if (it == m_entries.end() || std::next(it) == m_entries.end()) {
        return;
    }
    Entry entry = std::move(*it);
    m_entries.erase(it);
    m_entries.push_back(std::move(entry));
    rebuild_tree();
}

void CompositionHost::lower(WebView* owner) {
    const auto it = std::ranges::find(m_entries, owner, &Entry::owner);
    if (it == m_entries.end() || it == m_entries.begin()) {
        return;
    }
    Entry entry = std::move(*it);
    m_entries.erase(it);
    m_entries.insert(m_entries.begin(), std::move(entry));
    rebuild_tree();
}

void CompositionHost::rebuild_tree() noexcept {
    if (!m_root) {
        return;
    }
    m_root->RemoveAllVisuals();
    IDCompositionVisual* previous = nullptr;
    if (m_underlay) {
        BOSON_HR_CHECK(m_root->AddVisual(m_underlay.Get(), TRUE, nullptr));
        previous = m_underlay.Get();
    }
    for (const auto& entry : m_entries) {
        // insertAbove = TRUE relative to the previous sibling gives an explicit bottom -> top order.
        BOSON_HR_CHECK(m_root->AddVisual(entry.visual.Get(), TRUE, previous));
        previous = entry.visual.Get();
    }
    if (m_overlay) {
        BOSON_HR_CHECK(m_root->AddVisual(m_overlay.Get(), TRUE, previous));
    }
    commit();
}

bool CompositionHost::set_offset(IDCompositionVisual2* visual, int x, int y) const noexcept {
    if (visual == nullptr) {
        return false;
    }
    return BOSON_HR_CHECK(visual->SetOffsetX(static_cast<float>(x))) &&
           BOSON_HR_CHECK(visual->SetOffsetY(static_cast<float>(y)));
}

bool CompositionHost::set_opacity(IDCompositionVisual2* visual, float opacity) const noexcept {
    if (visual == nullptr) {
        return false;
    }
    ComPtr<IDCompositionVisual3> visual3;
    if (!BOSON_HR_CHECK(visual->QueryInterface(IID_PPV_ARGS(&visual3))) || !visual3) {
        return false;
    }
    return BOSON_HR_CHECK(visual3->SetOpacity(std::clamp(opacity, 0.0f, 1.0f)));
}

bool CompositionHost::set_clip(IDCompositionVisual2* visual, int width, int height, float radius) const noexcept {
    if (visual == nullptr || !m_device) {
        return false;
    }
    ComPtr<IDCompositionRectangleClip> clip;
    if (!BOSON_HR_CHECK(m_device->CreateRectangleClip(&clip)) || !clip) {
        return false;
    }
    clip->SetLeft(0.0f);
    clip->SetTop(0.0f);
    clip->SetRight(static_cast<float>(width));
    clip->SetBottom(static_cast<float>(height));
    if (radius > 0.0f) {
        clip->SetTopLeftRadiusX(radius);
        clip->SetTopLeftRadiusY(radius);
        clip->SetTopRightRadiusX(radius);
        clip->SetTopRightRadiusY(radius);
        clip->SetBottomLeftRadiusX(radius);
        clip->SetBottomLeftRadiusY(radius);
        clip->SetBottomRightRadiusX(radius);
        clip->SetBottomRightRadiusY(radius);
    }
    return BOSON_HR_CHECK(visual->SetClip(clip.Get()));
}

bool CompositionHost::set_visible(IDCompositionVisual2* visual, bool visible) const noexcept {
    ComPtr<IDCompositionVisual3> visual3;
    if (visual == nullptr || FAILED(visual->QueryInterface(IID_PPV_ARGS(&visual3))) || !visual3) {
        return false;
    }
    return BOSON_HR_CHECK(visual3->SetVisible(visible ? TRUE : FALSE));
}

bool CompositionHost::commit() const noexcept {
    if (!m_device) {
        return false;
    }
    return BOSON_HR_CHECK(m_device->Commit());
}

} // namespace boson::core
