#pragma once

#include <windows.h>
#include <wrl.h>
#include <d3d11.h>
#include <dwrite_2.h>

#include <boson/gpu/device.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace boson::gpu {

struct FrameContext;

/// Straight (non-premultiplied) RGBA in 0..1; premultiplied in the shader.
struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    /// 0xRRGGBBAA
    [[nodiscard]] static constexpr Color rgba(std::uint32_t rgba) noexcept {
        return Color{
            static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f,
            static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f,
            static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f,
            static_cast<float>(rgba & 0xFFu) / 255.0f,
        };
    }
};

struct RectF {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

/// @brief An RGBA8 texture for Renderer2D::draw_texture (or any D3D11 SRV wrapped with from_srv()).
struct Texture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t generation = 0; ///< Device::generation() it was created on
};

struct FontSpec {
    std::wstring family = L"Cascadia Mono"; ///< falls back to Consolas, then the first system family
    float size_px = 14.0f;                  ///< em size in physical pixels (multiply DIPs by dpi_scale)
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
};

struct GlyphMetrics {
    float cell_width = 0.0f;  ///< advance of '0' (monospace cell)
    float line_height = 0.0f; ///< ascent + descent + line gap, rounded up
    float ascent = 0.0f;
    float descent = 0.0f;
};

/// @brief DirectWrite glyph rasterizer + R8 GPU atlas (shelf packed, grayscale anti-aliasing). Glyphs are
/// rasterized on first use with IDWriteGlyphRunAnalysis; ASCII is a direct-indexed lookup, everything else a
/// hash map. When the atlas is full it is cleared (Renderer2D flushes first) and refilled on demand.
/// Not thread-safe; use it on the render thread (or with Device::lock() held).
class GlyphAtlas final : public DeviceResourceOwner {
public:
    struct Glyph {
        float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f; ///< atlas UVs
        float offset_x = 0.0f; ///< bitmap left relative to the pen position
        float offset_y = 0.0f; ///< bitmap top relative to the baseline (negative = above)
        float width = 0.0f;    ///< bitmap size in pixels (0 for blank glyphs such as space)
        float height = 0.0f;
        float advance = 0.0f;
    };

    GlyphAtlas(std::shared_ptr<Device> device, FontSpec font, std::uint32_t atlas_size = 1024);
    ~GlyphAtlas();

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;
    GlyphAtlas(GlyphAtlas&&) = delete;
    GlyphAtlas& operator=(GlyphAtlas&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_face != nullptr; }
    [[nodiscard]] const GlyphMetrics& metrics() const noexcept { return m_metrics; }
    [[nodiscard]] const FontSpec& font() const noexcept { return m_font; }
    [[nodiscard]] std::uint32_t atlas_size() const noexcept { return m_size; }
    /// @brief Bumped whenever the atlas is cleared; UVs from an older epoch are stale.
    [[nodiscard]] std::uint64_t epoch() const noexcept { return m_epoch; }
    [[nodiscard]] std::size_t glyph_count() const noexcept { return m_glyphs.size(); }

    /// @brief Looks up (rasterizing on a miss) the glyph for a code point. nullptr when the atlas is full: the
    /// caller flushes pending draws, calls clear() and asks again.
    [[nodiscard]] const Glyph* find(char32_t code_point);
    void clear();
    /// @brief Rasterizes code points in advance (e.g. printable ASCII at startup).
    void preload(std::u32string_view code_points);

    [[nodiscard]] ID3D11ShaderResourceView* srv();
    void release_device_resources() noexcept override;

private:
    bool create_font();
    bool ensure_texture();
    const Glyph* rasterize(char32_t code_point);

    std::shared_ptr<Device> m_device;
    FontSpec m_font;
    std::uint32_t m_size = 1024;
    GlyphMetrics m_metrics;
    float m_design_scale = 1.0f;

    Microsoft::WRL::ComPtr<IDWriteFactory2> m_factory;
    Microsoft::WRL::ComPtr<IDWriteFontFace> m_face;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    std::uint64_t m_texture_generation = 0;

    std::vector<Glyph> m_glyphs;
    std::array<std::int32_t, 128> m_ascii{};
    std::unordered_map<char32_t, std::int32_t> m_other;
    std::vector<std::uint8_t> m_scratch;
    std::uint32_t m_shelf_x = 1;
    std::uint32_t m_shelf_y = 1;
    std::uint32_t m_shelf_height = 0;
    std::uint64_t m_epoch = 1;
};

/// One terminal / editor cell.
struct TextCell {
    char32_t ch = U' ';
    std::uint32_t fg = 0xD4D4D4FFu; ///< 0xRRGGBBAA
    std::uint32_t bg = 0x00000000u; ///< 0xRRGGBBAA; alpha 0 = no background quad
};

/// @brief A cols x rows grid of TextCells: the terminal / editor fast path of Renderer2D::draw_text_grid
/// (one background run per same-colored span, one glyph quad per visible cell, no shaping).
class TextGrid {
public:
    TextGrid() = default;
    TextGrid(std::uint32_t cols, std::uint32_t rows) { resize(cols, rows); }

    void resize(std::uint32_t cols, std::uint32_t rows, TextCell fill = {});
    void fill(TextCell cell);
    [[nodiscard]] TextCell& at(std::uint32_t col, std::uint32_t row) noexcept { return m_cells[static_cast<std::size_t>(row) * m_cols + col]; }
    [[nodiscard]] const TextCell& at(std::uint32_t col, std::uint32_t row) const noexcept { return m_cells[static_cast<std::size_t>(row) * m_cols + col]; }
    /// @brief Writes UTF-8 text starting at (col,row), clipped to the row. Returns the columns written.
    std::uint32_t put(std::uint32_t col, std::uint32_t row, std::string_view utf8, std::uint32_t fg, std::uint32_t bg = 0);
    [[nodiscard]] std::uint32_t cols() const noexcept { return m_cols; }
    [[nodiscard]] std::uint32_t rows() const noexcept { return m_rows; }
    [[nodiscard]] std::span<const TextCell> cells() const noexcept { return m_cells; }

private:
    std::uint32_t m_cols = 0;
    std::uint32_t m_rows = 0;
    std::vector<TextCell> m_cells;
};

struct RendererStats {
    std::uint64_t quads = 0;      ///< since begin()
    std::uint64_t draw_calls = 0; ///< since begin()
};

/// @brief Tiny instanced 2D batch renderer on D3D11 (D3D11On12 on the D3D12 backend): solid and rounded rects
/// (SDF, anti-aliased), textured quads and DirectWrite glyphs from a GlyphAtlas, all in one dynamic instance
/// buffer drawn with as few DrawInstanced calls as texture switches allow. Shaders are compiled at build
/// time with fxc (shaders/batch2d.hlsl) and embedded as bytecode.
///
/// Use it on the render thread inside a frame callback (or with Device::lock() held). Device-dependent
/// objects are re-created lazily after a device-lost recovery.
class Renderer2D final : public DeviceResourceOwner {
public:
    explicit Renderer2D(std::shared_ptr<Device> device, std::uint32_t max_quads_per_batch = 16384);
    ~Renderer2D();

    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;
    Renderer2D(Renderer2D&&) = delete;
    Renderer2D& operator=(Renderer2D&&) = delete;

    /// @brief Targets the frame's back buffer; clears it when clear has a value (use alpha 0 for a fully
    /// transparent surface).
    bool begin(FrameContext& frame, std::optional<Color> clear = Color{0, 0, 0, 0});
    /// @brief Targets any render target view (offscreen textures, readback tests).
    bool begin(ID3D11RenderTargetView* target, std::uint32_t width, std::uint32_t height, std::optional<Color> clear);
    void end();

    void fill_rect(RectF rect, Color color);
    void fill_rounded_rect(RectF rect, float radius, Color color);
    /// @brief Rounded-rect outline of thickness border (inside the rect).
    void stroke_rounded_rect(RectF rect, float radius, float border, Color color);
    void draw_texture(const Texture& texture, RectF dst, RectF uv = {0, 0, 1, 1}, Color tint = {1, 1, 1, 1});
    void draw_texture(ID3D11ShaderResourceView* srv, RectF dst, RectF uv = {0, 0, 1, 1}, Color tint = {1, 1, 1, 1});
    /// @brief One line of UTF-8 text with the top of the line box at (x, y); simple advance layout, no shaping.
    /// Returns the pen x after the last glyph.
    float draw_text(GlyphAtlas& atlas, float x, float y, std::string_view utf8, Color color);
    /// @brief Draws grid with its top-left cell at (x, y), cells of atlas.metrics().cell_width x line_height.
    void draw_text_grid(GlyphAtlas& atlas, float x, float y, const TextGrid& grid);
    /// @brief Scissor rect in target pixels (std::nullopt = whole target). Flushes pending quads.
    void set_clip(std::optional<RectF> clip);

    /// @brief Creates an RGBA8 texture (straight alpha, rows of width * 4 bytes).
    [[nodiscard]] std::optional<Texture> create_texture(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba);

    [[nodiscard]] const RendererStats& stats() const noexcept { return m_stats; }
    void release_device_resources() noexcept override;

private:
    struct Instance {
        float rect[4];   // x, y, w, h (pixels)
        float uv[4];     // u0, v0, u1, v1
        float color[4];  // straight RGBA
        float params[4]; // radius, kind, border, 0
    };

    bool ensure_resources();
    void push(const Instance& instance);
    void flush();
    void bind_image(ID3D11ShaderResourceView* srv);
    const GlyphAtlas::Glyph* glyph(GlyphAtlas& atlas, char32_t cp);
    void use_atlas(GlyphAtlas& atlas);

    std::shared_ptr<Device> m_device;
    std::uint32_t m_capacity = 16384;
    std::uint64_t m_generation = 0;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_layout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_instances;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blend;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_raster;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler_point;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler_linear;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_white; ///< 1x1 placeholder for unbound slots

    std::vector<Instance> m_pending;
    ID3D11RenderTargetView* m_target = nullptr;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    ID3D11ShaderResourceView* m_image = nullptr;
    GlyphAtlas* m_atlas = nullptr;
    std::optional<RectF> m_clip;
    bool m_in_frame = false;
    RendererStats m_stats;
};

/// @brief Decodes one UTF-8 code point at text[i] and advances i (U+FFFD on malformed input).
[[nodiscard]] char32_t decode_utf8(std::string_view text, std::size_t& i) noexcept;

} // namespace boson::gpu
