#include <boson/gpu/renderer2d.hpp>
#include <boson/gpu/surface.hpp>

#include <boson/core/result.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

// fxc output (const BYTE g_batch2d_vs[] / g_batch2d_ps[]), generated at build time from shaders/batch2d.hlsl.
#include "batch2d_ps.h"
#include "batch2d_vs.h"

namespace boson::gpu {

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kKindSolid = 0.0f;
constexpr float kKindRounded = 1.0f;
constexpr float kKindGlyph = 2.0f;
constexpr float kKindImage = 3.0f;

} // namespace

char32_t decode_utf8(std::string_view text, std::size_t& i) noexcept {
    const auto byte = [&text](std::size_t at) { return static_cast<std::uint8_t>(text[at]); };
    const std::uint8_t lead = byte(i);
    if (lead < 0x80) {
        ++i;
        return lead;
    }
    std::size_t length = 0;
    char32_t cp = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        cp = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        cp = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        cp = lead & 0x07u;
    } else {
        ++i;
        return U'�';
    }
    if (i + length > text.size()) {
        i = text.size();
        return U'�';
    }
    for (std::size_t k = 1; k < length; ++k) {
        const std::uint8_t next = byte(i + k);
        if ((next & 0xC0) != 0x80) {
            i += k;
            return U'�';
        }
        cp = (cp << 6) | (next & 0x3Fu);
    }
    i += length;
    return cp;
}

// ---- TextGrid --------------------------------------------------------------------------------------------

void TextGrid::resize(std::uint32_t cols, std::uint32_t rows, TextCell fill_cell) {
    m_cols = cols;
    m_rows = rows;
    m_cells.assign(static_cast<std::size_t>(cols) * rows, fill_cell);
}

void TextGrid::fill(TextCell cell) {
    std::ranges::fill(m_cells, cell);
}

std::uint32_t TextGrid::put(std::uint32_t col, std::uint32_t row, std::string_view utf8, std::uint32_t fg, std::uint32_t bg) {
    if (row >= m_rows) {
        return 0;
    }
    std::uint32_t written = 0;
    std::size_t i = 0;
    while (i < utf8.size() && col + written < m_cols) {
        TextCell& cell = at(col + written, row);
        cell.ch = decode_utf8(utf8, i);
        cell.fg = fg;
        cell.bg = bg;
        ++written;
    }
    return written;
}

// ---- Renderer2D ------------------------------------------------------------------------------------------

Renderer2D::Renderer2D(std::shared_ptr<Device> device, std::uint32_t max_quads_per_batch)
    : m_device(std::move(device))
    , m_capacity(std::max(64u, max_quads_per_batch)) {
    m_pending.reserve(m_capacity);
    if (m_device) {
        m_device->add_resource_owner(this);
    }
}

Renderer2D::~Renderer2D() {
    if (m_device) {
        m_device->remove_resource_owner(this);
    }
}

void Renderer2D::release_device_resources() noexcept {
    m_vs.Reset();
    m_ps.Reset();
    m_layout.Reset();
    m_instances.Reset();
    m_constants.Reset();
    m_blend.Reset();
    m_raster.Reset();
    m_sampler_point.Reset();
    m_sampler_linear.Reset();
    m_white.Reset();
    m_pending.clear();
    m_target = nullptr;
    m_image = nullptr;
    m_atlas = nullptr;
    m_in_frame = false;
    m_generation = 0;
}

bool Renderer2D::ensure_resources() {
    if (!m_device) {
        return false;
    }
    if (m_vs && m_generation == m_device->generation() + 1) {
        return true;
    }
    release_device_resources();
    ID3D11Device* device = m_device->d3d11();
    if (device == nullptr) {
        return false;
    }
    if (!BOSON_HR_CHECK(device->CreateVertexShader(g_batch2d_vs, sizeof(g_batch2d_vs), nullptr, &m_vs)) ||
        !BOSON_HR_CHECK(device->CreatePixelShader(g_batch2d_ps, sizeof(g_batch2d_ps), nullptr, &m_ps))) {
        return false;
    }
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"UV", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"PARAMS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    if (!BOSON_HR_CHECK(device->CreateInputLayout(elements, 4, g_batch2d_vs, sizeof(g_batch2d_vs), &m_layout))) {
        return false;
    }
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(m_capacity * sizeof(Instance));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!BOSON_HR_CHECK(device->CreateBuffer(&desc, nullptr, &m_instances))) {
        return false;
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (!BOSON_HR_CHECK(device->CreateBuffer(&cb, nullptr, &m_constants))) {
        return false;
    }
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (!BOSON_HR_CHECK(device->CreateBlendState(&blend, &m_blend))) {
        return false;
    }
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.ScissorEnable = TRUE;
    raster.DepthClipEnable = TRUE;
    if (!BOSON_HR_CHECK(device->CreateRasterizerState(&raster, &m_raster))) {
        return false;
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (!BOSON_HR_CHECK(device->CreateSamplerState(&sampler, &m_sampler_point))) {
        return false;
    }
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (!BOSON_HR_CHECK(device->CreateSamplerState(&sampler, &m_sampler_linear))) {
        return false;
    }
    const std::uint32_t white = 0xFFFFFFFFu;
    D3D11_TEXTURE2D_DESC tex{};
    tex.Width = tex.Height = 1;
    tex.MipLevels = tex.ArraySize = 1;
    tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex.SampleDesc.Count = 1;
    tex.Usage = D3D11_USAGE_IMMUTABLE;
    tex.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{&white, 4, 4};
    ComPtr<ID3D11Texture2D> white_tex;
    if (!BOSON_HR_CHECK(device->CreateTexture2D(&tex, &init, &white_tex)) ||
        !BOSON_HR_CHECK(device->CreateShaderResourceView(white_tex.Get(), nullptr, &m_white))) {
        return false;
    }
    m_generation = m_device->generation() + 1;
    return true;
}

bool Renderer2D::begin(FrameContext& frame, std::optional<Color> clear) {
    return begin(frame.render_target, frame.width, frame.height, clear);
}

bool Renderer2D::begin(ID3D11RenderTargetView* target, std::uint32_t width, std::uint32_t height, std::optional<Color> clear) {
    if (target == nullptr || width == 0 || height == 0 || !ensure_resources()) {
        return false;
    }
    ID3D11DeviceContext* context = m_device->d3d11_context();
    m_target = target;
    m_width = width;
    m_height = height;
    m_image = nullptr;
    m_atlas = nullptr;
    m_clip.reset();
    m_pending.clear();
    m_stats = {};
    m_in_frame = true;

    const float constants[4] = {2.0f / static_cast<float>(width), 2.0f / static_cast<float>(height), 0.0f, 0.0f};
    context->UpdateSubresource(m_constants.Get(), 0, nullptr, constants, 0, 0);

    context->OMSetRenderTargets(1, &target, nullptr);
    const float blend_factor[4] = {0, 0, 0, 0};
    context->OMSetBlendState(m_blend.Get(), blend_factor, 0xFFFFFFFFu);
    context->OMSetDepthStencilState(nullptr, 0);
    D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    context->RSSetScissorRects(1, &scissor);
    context->RSSetState(m_raster.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->IASetInputLayout(m_layout.Get());
    const UINT stride = sizeof(Instance);
    const UINT offset = 0;
    ID3D11Buffer* buffers[] = {m_instances.Get()};
    context->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {m_constants.Get()};
    context->VSSetConstantBuffers(0, 1, cbs);
    context->PSSetShader(m_ps.Get(), nullptr, 0);
    ID3D11SamplerState* samplers[] = {m_sampler_point.Get(), m_sampler_linear.Get()};
    context->PSSetSamplers(0, 2, samplers);
    ID3D11ShaderResourceView* srvs[] = {m_white.Get(), m_white.Get()};
    context->PSSetShaderResources(0, 2, srvs);

    if (clear.has_value()) {
        const float premultiplied[4] = {clear->r * clear->a, clear->g * clear->a, clear->b * clear->a, clear->a};
        context->ClearRenderTargetView(target, premultiplied);
    }
    return true;
}

void Renderer2D::end() {
    if (!m_in_frame) {
        return;
    }
    flush();
    ID3D11DeviceContext* context = m_device->d3d11_context();
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    context->PSSetShaderResources(0, 2, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    m_in_frame = false;
    m_target = nullptr;
}

void Renderer2D::push(const Instance& instance) {
    if (!m_in_frame) {
        return;
    }
    if (m_pending.size() >= m_capacity) {
        flush();
    }
    m_pending.push_back(instance);
}

void Renderer2D::flush() {
    if (m_pending.empty() || !m_in_frame) {
        return;
    }
    ID3D11DeviceContext* context = m_device->d3d11_context();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!BOSON_HR_CHECK(context->Map(m_instances.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        m_pending.clear();
        return;
    }
    std::memcpy(mapped.pData, m_pending.data(), m_pending.size() * sizeof(Instance));
    context->Unmap(m_instances.Get(), 0);
    context->DrawInstanced(4, static_cast<UINT>(m_pending.size()), 0, 0);
    m_stats.quads += m_pending.size();
    ++m_stats.draw_calls;
    m_pending.clear();
}

void Renderer2D::bind_image(ID3D11ShaderResourceView* srv) {
    if (srv == m_image) {
        return;
    }
    flush();
    m_image = srv;
    ID3D11ShaderResourceView* view = srv != nullptr ? srv : m_white.Get();
    m_device->d3d11_context()->PSSetShaderResources(1, 1, &view);
}

void Renderer2D::use_atlas(GlyphAtlas& atlas) {
    if (m_atlas == &atlas) {
        return;
    }
    flush();
    m_atlas = &atlas;
    ID3D11ShaderResourceView* view = atlas.srv();
    if (view == nullptr) {
        view = m_white.Get();
    }
    m_device->d3d11_context()->PSSetShaderResources(0, 1, &view);
}

const GlyphAtlas::Glyph* Renderer2D::glyph(GlyphAtlas& atlas, char32_t cp) {
    const GlyphAtlas::Glyph* g = atlas.find(cp);
    if (g == nullptr) {
        flush(); // the quads already batched reference glyphs that are about to be evicted
        atlas.clear();
        g = atlas.find(cp);
    }
    return g;
}

void Renderer2D::set_clip(std::optional<RectF> clip) {
    if (!m_in_frame) {
        return;
    }
    flush();
    m_clip = clip;
    D3D11_RECT scissor{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    if (clip.has_value()) {
        scissor.left = std::max<LONG>(0, static_cast<LONG>(std::floor(clip->x)));
        scissor.top = std::max<LONG>(0, static_cast<LONG>(std::floor(clip->y)));
        scissor.right = std::min<LONG>(static_cast<LONG>(m_width), static_cast<LONG>(std::ceil(clip->x + clip->width)));
        scissor.bottom = std::min<LONG>(static_cast<LONG>(m_height), static_cast<LONG>(std::ceil(clip->y + clip->height)));
    }
    m_device->d3d11_context()->RSSetScissorRects(1, &scissor);
}

void Renderer2D::fill_rect(RectF rect, Color color) {
    push(Instance{{rect.x, rect.y, rect.width, rect.height}, {0, 0, 1, 1}, {color.r, color.g, color.b, color.a}, {0, kKindSolid, 0, 0}});
}

void Renderer2D::fill_rounded_rect(RectF rect, float radius, Color color) {
    push(Instance{{rect.x, rect.y, rect.width, rect.height}, {0, 0, 1, 1}, {color.r, color.g, color.b, color.a}, {radius, kKindRounded, 0, 0}});
}

void Renderer2D::stroke_rounded_rect(RectF rect, float radius, float border, Color color) {
    push(Instance{{rect.x, rect.y, rect.width, rect.height}, {0, 0, 1, 1}, {color.r, color.g, color.b, color.a},
                  {radius, kKindRounded, std::max(border, 0.001f), 0}});
}

void Renderer2D::draw_texture(const Texture& texture, RectF dst, RectF uv, Color tint) {
    if (texture.generation != m_device->generation()) {
        return; // created before a device-lost recovery: re-create it
    }
    draw_texture(texture.srv.Get(), dst, uv, tint);
}

void Renderer2D::draw_texture(ID3D11ShaderResourceView* srv, RectF dst, RectF uv, Color tint) {
    if (srv == nullptr || !m_in_frame) {
        return;
    }
    bind_image(srv);
    push(Instance{{dst.x, dst.y, dst.width, dst.height}, {uv.x, uv.y, uv.x + uv.width, uv.y + uv.height},
                  {tint.r, tint.g, tint.b, tint.a}, {0, kKindImage, 0, 0}});
}

float Renderer2D::draw_text(GlyphAtlas& atlas, float x, float y, std::string_view utf8, Color color) {
    if (!m_in_frame || !atlas.valid()) {
        return x;
    }
    use_atlas(atlas);
    const float baseline = std::round(y + atlas.metrics().ascent);
    float pen = x;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = decode_utf8(utf8, i);
        const GlyphAtlas::Glyph* g = glyph(atlas, cp);
        if (g == nullptr) {
            continue;
        }
        if (g->width > 0.0f) {
            push(Instance{{std::round(pen) + g->offset_x, baseline + g->offset_y, g->width, g->height},
                          {g->u0, g->v0, g->u1, g->v1},
                          {color.r, color.g, color.b, color.a},
                          {0, kKindGlyph, 0, 0}});
        }
        pen += g->advance;
    }
    return pen;
}

void Renderer2D::draw_text_grid(GlyphAtlas& atlas, float x, float y, const TextGrid& grid) {
    if (!m_in_frame || !atlas.valid() || grid.cols() == 0) {
        return;
    }
    use_atlas(atlas);
    const GlyphMetrics& m = atlas.metrics();
    const float cw = m.cell_width;
    const float lh = m.line_height;
    const float ascent = std::round(m.ascent);
    for (std::uint32_t row = 0; row < grid.rows(); ++row) {
        const float top = y + static_cast<float>(row) * lh;
        // Background runs: one quad per span of equal, non-transparent background.
        std::uint32_t col = 0;
        while (col < grid.cols()) {
            const std::uint32_t bg = grid.at(col, row).bg;
            std::uint32_t end = col + 1;
            while (end < grid.cols() && grid.at(end, row).bg == bg) {
                ++end;
            }
            if ((bg & 0xFFu) != 0) {
                fill_rect(RectF{x + static_cast<float>(col) * cw, top, static_cast<float>(end - col) * cw, lh}, Color::rgba(bg));
            }
            col = end;
        }
        const float baseline = std::round(top) + ascent;
        std::uint32_t last_fg = 0;
        Color fg{};
        for (col = 0; col < grid.cols(); ++col) {
            const TextCell& cell = grid.at(col, row);
            if (cell.ch == U' ' || cell.ch == 0 || (cell.fg & 0xFFu) == 0) {
                continue;
            }
            const GlyphAtlas::Glyph* g = glyph(atlas, cell.ch);
            if (g == nullptr || g->width <= 0.0f) {
                continue;
            }
            if (cell.fg != last_fg) {
                last_fg = cell.fg;
                fg = Color::rgba(cell.fg);
            }
            push(Instance{{std::round(x + static_cast<float>(col) * cw) + g->offset_x, baseline + g->offset_y, g->width, g->height},
                          {g->u0, g->v0, g->u1, g->v1},
                          {fg.r, fg.g, fg.b, fg.a},
                          {0, kKindGlyph, 0, 0}});
        }
    }
}

std::optional<Texture> Renderer2D::create_texture(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba) {
    if (!m_device || width == 0 || height == 0 || rgba.size() < static_cast<std::size_t>(width) * height * 4) {
        return std::nullopt;
    }
    const auto guard = m_device->lock();
    ID3D11Device* device = m_device->d3d11();
    if (device == nullptr) {
        return std::nullopt;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{rgba.data(), width * 4, 0};
    Texture out;
    if (!BOSON_HR_CHECK(device->CreateTexture2D(&desc, &init, &out.texture)) ||
        !BOSON_HR_CHECK(device->CreateShaderResourceView(out.texture.Get(), nullptr, &out.srv))) {
        return std::nullopt;
    }
    out.width = width;
    out.height = height;
    out.generation = m_device->generation();
    return out;
}

// ---- GlyphAtlas ------------------------------------------------------------------------------------------

GlyphAtlas::GlyphAtlas(std::shared_ptr<Device> device, FontSpec font, std::uint32_t atlas_size)
    : m_device(std::move(device))
    , m_font(std::move(font))
    , m_size(std::clamp(atlas_size, 64u, 8192u)) {
    m_ascii.fill(-1);
    if (m_device) {
        m_device->add_resource_owner(this);
    }
    create_font();
}

GlyphAtlas::~GlyphAtlas() {
    if (m_device) {
        m_device->remove_resource_owner(this);
    }
}

bool GlyphAtlas::create_font() {
    if (!BOSON_HR_CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                                            reinterpret_cast<IUnknown**>(m_factory.GetAddressOf())))) {
        return false;
    }
    ComPtr<IDWriteFontCollection> collection;
    if (!BOSON_HR_CHECK(m_factory->GetSystemFontCollection(&collection, FALSE))) {
        return false;
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    for (const wchar_t* family : {m_font.family.c_str(), L"Cascadia Mono", L"Consolas", L"Courier New"}) {
        if (SUCCEEDED(collection->FindFamilyName(family, &index, &exists)) && exists != FALSE) {
            break;
        }
    }
    if (exists == FALSE) {
        index = 0;
    }
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteFont> font;
    if (!BOSON_HR_CHECK(collection->GetFontFamily(index, &family)) ||
        !BOSON_HR_CHECK(family->GetFirstMatchingFont(m_font.weight, DWRITE_FONT_STRETCH_NORMAL, m_font.style, &font)) ||
        !BOSON_HR_CHECK(font->CreateFontFace(&m_face))) {
        m_face.Reset();
        return false;
    }
    DWRITE_FONT_METRICS fm{};
    m_face->GetMetrics(&fm);
    m_design_scale = m_font.size_px / static_cast<float>(fm.designUnitsPerEm);
    m_metrics.ascent = static_cast<float>(fm.ascent) * m_design_scale;
    m_metrics.descent = static_cast<float>(fm.descent) * m_design_scale;
    m_metrics.line_height = std::ceil(m_metrics.ascent + m_metrics.descent + static_cast<float>(fm.lineGap) * m_design_scale);
    const UINT32 zero = U'0';
    UINT16 glyph_index = 0;
    DWRITE_GLYPH_METRICS gm{};
    if (SUCCEEDED(m_face->GetGlyphIndices(&zero, 1, &glyph_index)) && SUCCEEDED(m_face->GetDesignGlyphMetrics(&glyph_index, 1, &gm, FALSE))) {
        m_metrics.cell_width = std::max(1.0f, std::round(static_cast<float>(gm.advanceWidth) * m_design_scale));
    } else {
        m_metrics.cell_width = std::max(1.0f, std::round(m_font.size_px * 0.6f));
    }
    return true;
}

bool GlyphAtlas::ensure_texture() {
    if (!m_device) {
        return false;
    }
    if (m_texture && m_texture_generation == m_device->generation() + 1) {
        return true;
    }
    if (m_texture) {
        release_device_resources();
    }
    ID3D11Device* device = m_device->d3d11();
    if (device == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = m_size;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (!BOSON_HR_CHECK(device->CreateTexture2D(&desc, nullptr, &m_texture)) ||
        !BOSON_HR_CHECK(device->CreateShaderResourceView(m_texture.Get(), nullptr, &m_srv))) {
        m_texture.Reset();
        m_srv.Reset();
        return false;
    }
    m_texture_generation = m_device->generation() + 1;
    return true;
}

void GlyphAtlas::release_device_resources() noexcept {
    m_srv.Reset();
    m_texture.Reset();
    m_texture_generation = 0;
    m_glyphs.clear();
    m_ascii.fill(-1);
    m_other.clear();
    m_shelf_x = m_shelf_y = 1;
    m_shelf_height = 0;
    ++m_epoch;
}

void GlyphAtlas::clear() {
    m_glyphs.clear();
    m_ascii.fill(-1);
    m_other.clear();
    m_shelf_x = m_shelf_y = 1;
    m_shelf_height = 0;
    ++m_epoch;
}

ID3D11ShaderResourceView* GlyphAtlas::srv() {
    return ensure_texture() ? m_srv.Get() : nullptr;
}

void GlyphAtlas::preload(std::u32string_view code_points) {
    for (const char32_t cp : code_points) {
        if (find(cp) == nullptr) {
            break;
        }
    }
}

const GlyphAtlas::Glyph* GlyphAtlas::find(char32_t code_point) {
    if (!ensure_texture()) {
        return nullptr;
    }
    if (code_point < m_ascii.size()) {
        if (const std::int32_t slot = m_ascii[code_point]; slot >= 0) {
            return &m_glyphs[static_cast<std::size_t>(slot)];
        }
    } else if (const auto it = m_other.find(code_point); it != m_other.end()) {
        return &m_glyphs[static_cast<std::size_t>(it->second)];
    }
    return rasterize(code_point);
}

const GlyphAtlas::Glyph* GlyphAtlas::rasterize(char32_t code_point) {
    if (!m_face) {
        return nullptr;
    }
    const UINT32 cp = code_point;
    UINT16 index = 0;
    m_face->GetGlyphIndices(&cp, 1, &index);
    DWRITE_GLYPH_METRICS gm{};
    m_face->GetDesignGlyphMetrics(&index, 1, &gm, FALSE);

    Glyph glyph;
    glyph.advance = static_cast<float>(gm.advanceWidth) * m_design_scale;

    const FLOAT advance = 0.0f;
    const DWRITE_GLYPH_OFFSET offset{};
    DWRITE_GLYPH_RUN run{};
    run.fontFace = m_face.Get();
    run.fontEmSize = m_font.size_px;
    run.glyphCount = 1;
    run.glyphIndices = &index;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &offset;
    ComPtr<IDWriteGlyphRunAnalysis> analysis;
    RECT bounds{};
    if (SUCCEEDED(m_factory->CreateGlyphRunAnalysis(&run, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                                                    DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_ENABLED,
                                                    DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0.0f, 0.0f, &analysis)) &&
        SUCCEEDED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds)) && bounds.right > bounds.left &&
        bounds.bottom > bounds.top) {
        const auto w = static_cast<std::uint32_t>(bounds.right - bounds.left);
        const auto h = static_cast<std::uint32_t>(bounds.bottom - bounds.top);
        if (w + 2 > m_size || h + 2 > m_size) {
            return nullptr;
        }
        if (m_shelf_x + w + 1 > m_size) {
            m_shelf_y += m_shelf_height + 1;
            m_shelf_x = 1;
            m_shelf_height = 0;
        }
        if (m_shelf_y + h + 1 > m_size) {
            return nullptr; // full: the renderer flushes and clears
        }
        m_scratch.resize(static_cast<std::size_t>(w) * h);
        if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, m_scratch.data(),
                                                static_cast<UINT32>(m_scratch.size())))) {
            return nullptr;
        }
        const D3D11_BOX box{m_shelf_x, m_shelf_y, 0, m_shelf_x + w, m_shelf_y + h, 1};
        m_device->d3d11_context()->UpdateSubresource(m_texture.Get(), 0, &box, m_scratch.data(), w, 0);
        const auto size = static_cast<float>(m_size);
        glyph.u0 = static_cast<float>(m_shelf_x) / size;
        glyph.v0 = static_cast<float>(m_shelf_y) / size;
        glyph.u1 = static_cast<float>(m_shelf_x + w) / size;
        glyph.v1 = static_cast<float>(m_shelf_y + h) / size;
        glyph.offset_x = static_cast<float>(bounds.left);
        glyph.offset_y = static_cast<float>(bounds.top);
        glyph.width = static_cast<float>(w);
        glyph.height = static_cast<float>(h);
        m_shelf_x += w + 1;
        m_shelf_height = std::max(m_shelf_height, h);
    }

    const auto slot = static_cast<std::int32_t>(m_glyphs.size());
    m_glyphs.push_back(glyph);
    if (code_point < m_ascii.size()) {
        m_ascii[code_point] = slot;
    } else {
        m_other.emplace(code_point, slot);
    }
    return &m_glyphs.back();
}

} // namespace boson::gpu
