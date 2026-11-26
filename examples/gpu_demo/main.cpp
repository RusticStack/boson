// boson_gpu_demo: native GPU content composited with WebView2.
//   * "terminal": an animated text grid (Renderer2D + DirectWrite glyph atlas) in a swap chain BELOW the
//     transparent WebView, filling the window; a semi-transparent HTML panel floats above it.
//   * "vulkan":   Vulkan renders animated tiles into a shared image (D3D NT handle + shared fence); the frame
//     callback samples it into the surface - the same presentation path as native D3D content. Inside a
//     scrollable HTML card, so the surface is clipped by the scroll container.
//   * "badge":    a small native overlay ABOVE the WebView.
// Flags: --windowed (child-HWND WebView hosting instead of composition), --d3d12 (D3D12 device).
#include <windows.h>
#include <objbase.h>

#include <boson/boson.hpp>
#include <boson/gpu/device.hpp>
#include <boson/gpu/renderer2d.hpp>
#include <boson/gpu/surface.hpp>
#include <boson/gpu/vulkan_interop.hpp>
#include <boson/ipc/controllers/gpu_controller.hpp>

#include "demo_page.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace boson;

class ComApartment {
public:
    ComApartment() noexcept : m_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(m_hr)) {
            CoUninitialize();
        }
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    HRESULT m_hr;
};

/// Animated pseudo terminal: a scrolling build log in a TextGrid, re-rendered every frame.
class TerminalContent {
public:
    explicit TerminalContent(std::shared_ptr<gpu::Device> device)
        : m_device(device), m_renderer(device) {
        static constexpr std::array<const char*, 6> kModules{"core", "gpu", "ipc", "shell", "transfer", "webview"};
        static constexpr std::array<const char*, 6> kFiles{"surface.cpp", "render_loop.cpp", "renderer2d.cpp", "device.cpp",
                                                           "vulkan_interop.cpp", "composition.cpp"};
        for (int i = 0; i < 96; ++i) {
            const int kind = (i * 7) % 11;
            const char* level = kind == 0 ? "WARN " : kind == 5 ? "ERROR" : "INFO ";
            m_lines.push_back(Line{
                .stamp = std::format("[12:{:02}:{:02}.{:03}]", (i / 60) % 60, i % 60, (i * 137) % 1000),
                .level = level,
                .text = std::format(" {:<8} compiling src/{}/{} ({}%)  cl /std:c++latest /W4 /O2", kModules[static_cast<std::size_t>(i) % 6],
                                    kModules[static_cast<std::size_t>(i) % 6], kFiles[static_cast<std::size_t>(i * 5) % 6], (i * 13) % 101),
                .color = kind == 0 ? 0xFFD43BFFu : kind == 5 ? 0xFF6B6BFFu : 0x69DB7CFFu,
            });
        }
    }

    void frame(gpu::FrameContext& f) {
        if (!m_atlas || m_scale != f.dpi_scale) {
            m_scale = f.dpi_scale;
            m_atlas = std::make_unique<gpu::GlyphAtlas>(m_device, gpu::FontSpec{.size_px = std::round(15.0f * m_scale)});
        }
        const gpu::GlyphMetrics& m = m_atlas->metrics();
        const auto cols = static_cast<std::uint32_t>(static_cast<float>(f.width) / m.cell_width) + 1;
        const auto rows = static_cast<std::uint32_t>(static_cast<float>(f.height) / m.line_height) + 1;
        if (cols != m_grid.cols() || rows != m_grid.rows()) {
            m_grid.resize(cols, rows);
        }
        m_fps = f.delta_seconds > 0 ? m_fps * 0.95 + (1.0 / f.delta_seconds) * 0.05 : m_fps;
        const double scroll = f.time_seconds * 6.0;
        const auto first = static_cast<std::size_t>(scroll);
        const float offset = -static_cast<float>(scroll - std::floor(scroll)) * m.line_height;
        m_grid.fill(gpu::TextCell{});
        for (std::uint32_t row = 1; row < rows; ++row) {
            const Line& line = m_lines[(first + row) % m_lines.size()];
            std::uint32_t col = 1;
            col += m_grid.put(col, row, line.stamp, 0x868E96FFu);
            col += m_grid.put(col + 1, row, line.level, line.color) + 1;
            m_grid.put(col, row, line.text, 0xDEE2E6FFu);
        }
        const std::string status = std::format(" boson_gpu_demo  {}x{} px  {:.0f} fps  frame {}  cpu {:.2f} ms ", f.width, f.height, m_fps,
                                               f.frame_index, m_last_cpu_ms);
        // Blinking block cursor on the last line.
        if (static_cast<int>(f.time_seconds * 2.0) % 2 == 0) {
            m_grid.at(1, rows - 2).bg = 0xE9ECEFFFu;
        }

        const LARGE_INTEGER start = now();
        m_renderer.begin(f, gpu::Color{0.035f, 0.04f, 0.06f, 1.0f});
        const float sweep = static_cast<float>(std::fmod(f.time_seconds * 0.25, 1.0)) * static_cast<float>(f.width);
        m_renderer.fill_rect({sweep - 160.0f, 0, 160.0f, static_cast<float>(f.height)}, gpu::Color{0.2f, 0.4f, 1.0f, 0.06f});
        m_renderer.draw_text_grid(*m_atlas, 0, offset, m_grid); // row 0 stays empty: the status bar covers it
        // Status bar, unscrolled, on top.
        m_renderer.fill_rect({0, 0, static_cast<float>(f.width), m.line_height}, gpu::Color::rgba(0x74C0FCFFu));
        gpu::TextGrid header(cols, 1);
        header.put(0, 0, status, 0x101318FFu, 0x74C0FCFFu);
        m_renderer.draw_text_grid(*m_atlas, 0, 0, header);
        m_renderer.end();
        m_last_cpu_ms = elapsed_ms(start);
        f.surface.request_frame(); // animate: paced by the swap chain / compositor clock
    }

private:
    struct Line {
        std::string stamp;
        std::string level;
        std::string text;
        std::uint32_t color = 0;
    };

    static LARGE_INTEGER now() {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return t;
    }
    static double elapsed_ms(LARGE_INTEGER start) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(now().QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
    }

    std::shared_ptr<gpu::Device> m_device;
    gpu::Renderer2D m_renderer;
    std::unique_ptr<gpu::GlyphAtlas> m_atlas;
    float m_scale = 0.0f;
    gpu::TextGrid m_grid;
    std::vector<Line> m_lines;
    double m_fps = 60.0;
    double m_last_cpu_ms = 0.0;
};

/// The Vulkan interop is created once, off the UI and render threads (it can take a few hundred ms).
struct VulkanState {
    std::mutex mutex;
    bool started = false;
    bool done = false;
    std::shared_ptr<gpu::VulkanInterop> interop;
    std::string why_not;
};

class VulkanContent {
public:
    VulkanContent(std::shared_ptr<gpu::Device> device, std::shared_ptr<VulkanState> state)
        : m_device(device), m_state(std::move(state)), m_renderer(device), m_atlas(device, gpu::FontSpec{.size_px = 16.0f}) {}

    void frame(gpu::FrameContext& f) {
        std::shared_ptr<gpu::VulkanInterop> interop;
        std::string why;
        bool done = false;
        {
            std::scoped_lock guard(m_state->mutex);
            interop = m_state->interop;
            why = m_state->why_not;
            done = m_state->done;
        }
        if (!interop) {
            m_renderer.begin(f, gpu::Color{0.12f, 0.08f, 0.08f, 1.0f});
            m_renderer.draw_text(m_atlas, 12, 12, done ? "Vulkan unavailable: " + why : std::string("starting Vulkan..."),
                                 gpu::Color{1, 0.8f, 0.8f, 1});
            m_renderer.end();
            if (!done) {
                f.surface.request_frame();
            }
            return;
        }
        if (!m_image || m_image->width() != f.width || m_image->height() != f.height) {
            m_image = interop->create_image(f.width, f.height);
        }
        if (!m_image || !m_image->begin_vulkan()) {
            m_image.reset();
            f.surface.request_frame();
            return;
        }
        // Vulkan: animated tiles via vkCmdClearAttachments inside a render pass.
        const auto t = static_cast<float>(f.time_seconds);
        const std::uint32_t tile = 28;
        m_rects.clear();
        for (std::uint32_t y = 0; y < f.height; y += tile) {
            for (std::uint32_t x = 0; x < f.width; x += tile) {
                const float fx = static_cast<float>(x) * 0.012f;
                const float fy = static_cast<float>(y) * 0.017f;
                const float v = 0.5f + 0.5f * std::sin(fx + fy + t * 2.2f);
                const float w = 0.5f + 0.5f * std::sin(fx * 1.7f - t * 1.3f);
                m_rects.push_back(gpu::VulkanClearRect{
                    .x = static_cast<std::int32_t>(x + 1), .y = static_cast<std::int32_t>(y + 1), .width = tile - 2, .height = tile - 2,
                    .rgba = {0.15f + 0.6f * v, 0.1f + 0.35f * w, 0.55f + 0.45f * (1.0f - v), 1.0f}});
            }
        }
        const float background[4] = {0.02f, 0.02f, 0.05f, 1.0f};
        m_image->clear_rects(background, m_rects);
        m_image->end_vulkan();
        // D3D: wait on the shared fence, sample the image, annotate, signal back.
        m_image->acquire_d3d();
        m_renderer.begin(f, std::nullopt);
        m_renderer.draw_texture(m_image->srv(), {0, 0, static_cast<float>(f.width), static_cast<float>(f.height)});
        m_renderer.fill_rounded_rect({8, 8, 330, 28}, 8, gpu::Color{0, 0, 0, 0.6f});
        m_renderer.draw_text(m_atlas, 16, 12,
                             m_image->fence_shared() ? "Vulkan -> D3D shared image + shared fence" : "Vulkan -> D3D shared image (CPU sync)",
                             gpu::Color{1, 1, 1, 1});
        m_renderer.end();
        m_image->release_d3d();
        f.surface.request_frame();
    }

private:
    std::shared_ptr<gpu::Device> m_device;
    std::shared_ptr<VulkanState> m_state;
    gpu::Renderer2D m_renderer;
    gpu::GlyphAtlas m_atlas;
    std::unique_ptr<gpu::SharedImage> m_image;
    std::vector<gpu::VulkanClearRect> m_rects;
};

/// Native overlay above the WebView: redrawn only when the second changes (on demand, not every frame).
class BadgeContent {
public:
    explicit BadgeContent(std::shared_ptr<gpu::Device> device)
        : m_renderer(device), m_atlas(device, gpu::FontSpec{.family = L"Segoe UI", .size_px = 15.0f}) {}

    void frame(gpu::FrameContext& f) {
        m_renderer.begin(f, gpu::Color{0, 0, 0, 0});
        const auto w = static_cast<float>(f.width);
        const auto h = static_cast<float>(f.height);
        m_renderer.fill_rounded_rect({0, 0, w, h}, 12.0f * f.dpi_scale, gpu::Color{0.95f, 0.45f, 0.1f, 0.92f});
        m_renderer.stroke_rounded_rect({0, 0, w, h}, 12.0f * f.dpi_scale, 2.0f, gpu::Color{1, 1, 1, 0.8f});
        m_renderer.draw_text(m_atlas, 12, 8, "native overlay (above WebView)", gpu::Color{1, 1, 1, 1});
        m_renderer.draw_text(m_atlas, 12, 30, std::format("redrawn on demand: frame {}", f.frame_index), gpu::Color{1, 1, 1, 0.85f});
        m_renderer.end();
    }

private:
    gpu::Renderer2D m_renderer;
    gpu::GlyphAtlas m_atlas;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    const ComApartment com;
    bool windowed = false;
    bool d3d12 = false;
    for (const std::wstring& arg : app::command_line_args()) {
        windowed = windowed || arg == L"--windowed";
        d3d12 = d3d12 || arg == L"--d3d12";
    }

    core::Window window(core::WindowConfig{.title = L"Boson GPU demo", .width = 1280, .height = 800, .dark_mode = true});
    if (!window.create(instance, show_cmd)) {
        return 1;
    }
    boson::exec::Scheduler scheduler;
    boson::exec::UiMarshal marshal([&window]() { window.post_invoke(); });
    window.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    const auto environment = core::Environment::create({});
    core::WebViewHost host(window, environment, windowed ? core::HostingMode::Windowed : core::HostingMode::Composition);
    gpu::SurfaceHost surfaces(host);

    core::WebViewConfig config;
    config.transparent_background = true; // native content shows through transparent page regions
    config.scripts_on_document_created.push_back(ipc::GpuController::page_script());
    core::WebView& webview = host.create(std::move(config), core::WebViewCallbacks{
        .asset_lookup = [](std::string_view path) -> std::optional<std::span<const std::byte>> {
            if (path == "/" || path == "/index.html" || path == "index.html") {
                return std::as_bytes(std::span{gpu_demo::kIndexHtml.data(), gpu_demo::kIndexHtml.size()});
            }
            return std::nullopt;
        },
    });

    ipc::GpuController gpu_controller(surfaces, marshal, &webview, gpu::DeviceOptions{.backend = d3d12 ? gpu::Backend::D3D12 : gpu::Backend::D3D11});
    auto vulkan = std::make_shared<VulkanState>();
    gpu_controller.register_content("terminal", [](gpu::Surface& surface) {
        auto content = std::make_shared<TerminalContent>(surface.device());
        surface.set_frame_callback([content](gpu::FrameContext& f) { content->frame(f); });
    });
    gpu_controller.register_content("badge", [](gpu::Surface& surface) {
        auto content = std::make_shared<BadgeContent>(surface.device());
        surface.set_frame_callback([content](gpu::FrameContext& f) { content->frame(f); });
    });
    gpu_controller.register_content("vulkan", [vulkan, &runner](gpu::Surface& surface) {
        std::shared_ptr<gpu::Device> device = surface.device();
        {
            std::scoped_lock guard(vulkan->mutex);
            if (!vulkan->started) {
                vulkan->started = true;
                runner.spawn([vulkan, device]() {
                    std::string why;
                    std::shared_ptr<gpu::VulkanInterop> interop = gpu::VulkanInterop::create(device, &why);
                    std::scoped_lock lock(vulkan->mutex);
                    vulkan->interop = std::move(interop);
                    vulkan->why_not = std::move(why);
                    vulkan->done = true;
                });
            }
        }
        auto content = std::make_shared<VulkanContent>(device, vulkan);
        surface.set_frame_callback([content](gpu::FrameContext& f) { content->frame(f); });
    });

    ipc::Router<ipc::GpuController> router(gpu_controller);
    ipc::Bridge bridge(runner, marshal, router);
    bridge.attach(webview);
    gpu_controller.connect(bridge);

    bool quitting = false;
    window.on(core::WindowEventKind::CloseRequested, [&](core::WindowEvent& e) {
        if (quitting) {
            return;
        }
        quitting = true;
        e.cancel = true;
        window.hide();
        app::quit({environment});
    });
    return core::Window::run_message_loop();
}
