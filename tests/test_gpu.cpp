// Native GPU surfaces (boson_gpu): devices, offscreen readback of the 2D batch renderer, DirectWrite glyphs,
// composition swap chains (create / resize / present / minimize / idle), device-lost recovery, Vulkan interop
// readback and the Gpu IPC controller. Opens small SW_SHOWNOACTIVATE windows; nothing is left behind.
#include <windows.h>
#include <objbase.h>

#include <boson/core/composition.hpp>
#include <boson/core/window.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/gpu/device.hpp>
#include <boson/gpu/render_loop.hpp>
#include <boson/gpu/renderer2d.hpp>
#include <boson/gpu/surface.hpp>
#include <boson/gpu/vulkan_interop.hpp>
#include <boson/ipc/controllers/gpu_controller.hpp>
#include <boson/ipc/router.hpp>

#include <glaze/glaze.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using namespace boson;
using Clock = std::chrono::steady_clock;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_gpu.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define GPU_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

void report(std::string_view what, double value, std::string_view unit) {
    std::printf("[perf] %-58.*s %10.3f %.*s\n", static_cast<int>(what.size()), what.data(), value, static_cast<int>(unit.size()),
                unit.data());
    std::fflush(stdout);
}

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void pump_once() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    const auto deadline = Clock::now() + timeout;
    while (!done()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
        pump_once();
    }
    return true;
}

void pump_for(std::chrono::milliseconds duration) {
    const auto until = Clock::now() + duration;
    pump_until([until]() { return Clock::now() >= until; }, duration + std::chrono::seconds{1});
}

// ---- Readback helpers ------------------------------------------------------------------------------------

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint32_t> pixels; // BGRA8 little endian: 0xAARRGGBB

    [[nodiscard]] std::uint32_t at(std::uint32_t x, std::uint32_t y) const { return pixels[static_cast<std::size_t>(y) * width + x]; }
};

std::uint32_t channel(std::uint32_t pixel, int shift) {
    return (pixel >> shift) & 0xFFu;
}

bool close_to(std::uint32_t pixel, std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t tolerance = 3) {
    const auto close = [tolerance](std::uint32_t a, std::uint32_t e) { return (a > e ? a - e : e - a) <= tolerance; };
    return close(channel(pixel, 16), r) && close(channel(pixel, 8), g) && close(channel(pixel, 0), b);
}

/// Copies a BGRA texture to a staging texture and maps it. Device lock must be held (or taken here).
std::optional<Image> read_texture(gpu::Device& device, ID3D11Texture2D* texture) {
    const auto guard = device.lock();
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device.d3d11()->CreateTexture2D(&desc, nullptr, &staging))) {
        return std::nullopt;
    }
    ID3D11DeviceContext* context = device.d3d11_context();
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return std::nullopt;
    }
    Image image{desc.Width, desc.Height, std::vector<std::uint32_t>(static_cast<std::size_t>(desc.Width) * desc.Height)};
    for (std::uint32_t y = 0; y < desc.Height; ++y) {
        std::memcpy(&image.pixels[static_cast<std::size_t>(y) * desc.Width], static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                    desc.Width * 4u);
    }
    context->Unmap(staging.Get(), 0);
    return image;
}

struct Target {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
};

std::optional<Target> make_target(gpu::Device& device, std::uint32_t width, std::uint32_t height) {
    const auto guard = device.lock();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Target t;
    if (FAILED(device.d3d11()->CreateTexture2D(&desc, nullptr, &t.texture)) ||
        FAILED(device.d3d11()->CreateRenderTargetView(t.texture.Get(), nullptr, &t.rtv))) {
        return std::nullopt;
    }
    return t;
}

// ---- Tests -----------------------------------------------------------------------------------------------

std::shared_ptr<gpu::Device> test_device(gpu::Backend backend, gpu::AdapterPreference pref, std::string_view label) {
    step(std::string("device ") + std::string(label));
    const auto start = Clock::now();
    auto device = gpu::Device::create({.backend = backend, .adapter = pref});
    const double elapsed = ms_since(start);
    GPU_CHECK(device != nullptr);
    if (!device) {
        return nullptr;
    }
    const gpu::DeviceInfo info = device->info();
    std::cout << "  " << info.backend << " on '" << info.adapter.name << "' FL " << info.feature_level
              << " vram " << (info.adapter.dedicated_video_memory >> 20) << " MiB tearing " << info.tearing_supported
              << " compositor_clock " << info.compositor_clock << '\n';
    report(std::string("Device::create ") + std::string(label), elapsed, "ms");
    GPU_CHECK(!info.adapter.name.empty());
    GPU_CHECK(info.feature_level_value >= static_cast<std::uint32_t>(D3D_FEATURE_LEVEL_10_0));
    GPU_CHECK(device->d3d11() != nullptr);
    GPU_CHECK(device->d3d11_context() != nullptr);
    GPU_CHECK(device->removed_reason() == S_OK);
    if (backend == gpu::Backend::D3D12) {
        GPU_CHECK(device->d3d12() != nullptr);
        GPU_CHECK(device->d3d12_queue() != nullptr);
        GPU_CHECK(device->d3d11on12() != nullptr);
        GPU_CHECK(info.backend == "d3d12");
    }
    if (pref == gpu::AdapterPreference::Warp) {
        GPU_CHECK(info.adapter.software);
    }
    return device;
}

void test_offscreen(const std::shared_ptr<gpu::Device>& device, std::string_view label) {
    step(std::string("offscreen readback ") + std::string(label));
    constexpr std::uint32_t kW = 160;
    constexpr std::uint32_t kH = 64;
    auto target = make_target(*device, kW, kH);
    GPU_CHECK(target.has_value());
    if (!target) {
        return;
    }
    gpu::Renderer2D renderer(device);
    gpu::GlyphAtlas atlas(device, gpu::FontSpec{.size_px = 24.0f});
    GPU_CHECK(atlas.valid());
    GPU_CHECK(atlas.metrics().cell_width > 0.0f);
    GPU_CHECK(atlas.metrics().line_height >= 24.0f);

    const std::uint8_t checker[16] = {255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255}; // 2x2 yellow
    {
        const auto guard = device->lock();
        const auto texture = renderer.create_texture(2, 2, checker);
        GPU_CHECK(texture.has_value());
        GPU_CHECK(renderer.begin(target->rtv.Get(), kW, kH, gpu::Color{0, 0, 0, 1}));
        renderer.fill_rect({10, 10, 20, 20}, gpu::Color::rgba(0xFF0000FFu));
        renderer.fill_rounded_rect({40, 10, 30, 30}, 10.0f, gpu::Color::rgba(0x00FF00FFu));
        if (texture) {
            renderer.draw_texture(*texture, {4, 44, 16, 16});
        }
        renderer.draw_text(atlas, 80, 8, "Hg", gpu::Color{1, 1, 1, 1});
        renderer.set_clip(gpu::RectF{130, 0, 10, 64});
        renderer.fill_rect({120, 40, 40, 20}, gpu::Color::rgba(0x0000FFFFu)); // clipped to x in [130, 140)
        renderer.set_clip(std::nullopt);
        renderer.end();
        GPU_CHECK(renderer.stats().quads == 6);
        GPU_CHECK(renderer.stats().draw_calls >= 2);
    }
    const auto image = read_texture(*device, target->texture.Get());
    GPU_CHECK(image.has_value());
    if (!image) {
        return;
    }
    GPU_CHECK(close_to(image->at(20, 20), 255, 0, 0));   // solid rect
    GPU_CHECK(close_to(image->at(5, 5), 0, 0, 0));       // clear color
    GPU_CHECK(close_to(image->at(55, 25), 0, 255, 0));   // rounded rect center
    GPU_CHECK(close_to(image->at(40, 10), 0, 0, 0, 40)); // rounded corner stays (almost) background
    GPU_CHECK(close_to(image->at(12, 52), 255, 255, 0)); // textured quad
    GPU_CHECK(close_to(image->at(135, 50), 0, 0, 255));  // inside the scissor
    GPU_CHECK(close_to(image->at(125, 50), 0, 0, 0));    // scissored away
    // Glyphs: some fully covered pixels and anti-aliased edge pixels in the text box.
    int bright = 0;
    int partial = 0;
    for (std::uint32_t y = 8; y < 8 + 34; ++y) {
        for (std::uint32_t x = 80; x < 125; ++x) {
            const std::uint32_t v = channel(image->at(x, y), 8);
            bright += v > 200 ? 1 : 0;
            partial += (v > 20 && v < 200) ? 1 : 0;
        }
    }
    std::cout << "  glyph pixels: " << bright << " solid, " << partial << " anti-aliased; atlas glyphs " << atlas.glyph_count() << '\n';
    GPU_CHECK(bright > 20);
    GPU_CHECK(partial > 10);
    GPU_CHECK(atlas.glyph_count() == 2);
}

void bench_text_grid(const std::shared_ptr<gpu::Device>& device, std::string_view label) {
    step(std::string("bench text grid ") + std::string(label));
    gpu::GlyphAtlas atlas(device, gpu::FontSpec{.size_px = 14.0f});
    const std::uint32_t cols = 240;
    const std::uint32_t rows = 70;
    const auto w = static_cast<std::uint32_t>(std::ceil(atlas.metrics().cell_width * static_cast<float>(cols)));
    const auto h = static_cast<std::uint32_t>(atlas.metrics().line_height * static_cast<float>(rows));
    auto target = make_target(*device, w, h);
    GPU_CHECK(target.has_value());
    if (!target) {
        return;
    }
    gpu::Renderer2D renderer(device);
    gpu::TextGrid grid(cols, rows);
    std::uint32_t seed = 12345;
    const auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (std::uint32_t r = 0; r < rows; ++r) {
        for (std::uint32_t c = 0; c < cols; ++c) {
            auto& cell = grid.at(c, r);
            cell.ch = static_cast<char32_t>(33 + rnd() % 94);
            cell.fg = (rnd() << 8) | 0xFFu;
            cell.bg = (c / 16) % 3 == 0 ? 0x202020FFu : 0;
        }
    }
    constexpr int kFrames = 60;
    double cpu_ms = 0;
    double total_ms = 0;
    for (int i = 0; i < kFrames + 5; ++i) {
        const auto start = Clock::now();
        {
            const auto guard = device->lock();
            renderer.begin(target->rtv.Get(), w, h, gpu::Color{0.05f, 0.05f, 0.07f, 1});
            renderer.draw_text_grid(atlas, 0, 0, grid);
            renderer.end();
        }
        const double cpu = ms_since(start);
        device->wait_idle();
        if (i >= 5) {
            cpu_ms += cpu;
            total_ms += ms_since(start);
        }
    }
    std::cout << "  " << cols << "x" << rows << " cells at " << w << "x" << h << ", " << renderer.stats().quads << " quads in "
              << renderer.stats().draw_calls << " draw calls\n";
    report(std::string("text grid 240x70 CPU record+submit ") + std::string(label), cpu_ms / kFrames, "ms/frame");
    report(std::string("text grid 240x70 CPU+GPU (wait idle) ") + std::string(label), total_ms / kFrames, "ms/frame");
    GPU_CHECK(renderer.stats().quads >= static_cast<std::uint64_t>(cols) * rows);
}

struct TestWindow {
    core::Window window;
    explicit TestWindow(const wchar_t* title)
        : window(core::WindowConfig{.title = title, .width = 420, .height = 300, .quit_on_destroy = false}) {}
};

/// Surface create / present / readback / resize / idle / minimize on one host.
void test_surface(const std::shared_ptr<gpu::Device>& device, gpu::SurfaceHost& host, core::Window& window, std::string_view label) {
    step(std::string("surface ") + std::string(label));
    std::atomic<int> animate{0};
    std::atomic<bool> capture{false};
    std::atomic<std::uint32_t> last_width{0};
    std::optional<Image> captured;
    std::mutex captured_mutex;
    auto renderer = std::make_shared<gpu::Renderer2D>(device);

    const auto created = Clock::now();
    auto surface = host.create_surface(device, gpu::SurfaceOptions{.bounds = RECT{10, 10, 210, 110}});
    GPU_CHECK(surface != nullptr);
    if (!surface) {
        return;
    }
    gpu::Surface* raw = surface.get();
    surface->set_frame_callback([&, renderer, raw](gpu::FrameContext& frame) {
        renderer->begin(frame, gpu::Color{0.0f, 0.5f, 1.0f, 1.0f});
        renderer->fill_rect({0, 0, 8, 8}, gpu::Color::rgba(0xFF0000FFu));
        renderer->end();
        last_width = frame.width;
        if (capture.exchange(false)) {
            auto image = read_texture(frame.device, frame.back_buffer);
            std::scoped_lock guard(captured_mutex);
            captured = std::move(image);
        }
        if (animate.load() > 0) {
            animate.fetch_sub(1);
            raw->request_frame();
        }
    });
    surface->request_frame();
    GPU_CHECK(pump_until([&]() { return surface->stats().frames_presented >= 1; }));
    report(std::string("create_surface -> first present ") + std::string(label), ms_since(created), "ms");
    GPU_CHECK(surface->stats().swap_chain_creations == 1);
    GPU_CHECK(last_width.load() == 200);

    // Readback of what was rendered into the swap chain back buffer.
    capture = true;
    surface->request_frame();
    GPU_CHECK(pump_until([&]() { std::scoped_lock g(captured_mutex); return captured.has_value(); }));
    {
        std::scoped_lock guard(captured_mutex);
        if (captured) {
            GPU_CHECK(captured->width == 200 && captured->height == 100);
            GPU_CHECK(close_to(captured->at(100, 50), 0, 127, 255, 2));
            GPU_CHECK(close_to(captured->at(2, 2), 255, 0, 0));
        }
        captured.reset();
    }

    // Request coalescing: many requests before the render thread runs -> few frames.
    const auto before = surface->stats().frames_presented;
    for (int i = 0; i < 100; ++i) {
        surface->request_frame();
    }
    pump_for(std::chrono::milliseconds{100});
    const auto coalesced = surface->stats().frames_presented - before;
    std::cout << "  100 request_frame() calls -> " << coalesced << " frame(s)\n";
    GPU_CHECK(coalesced >= 1 && coalesced <= 3);

    // Idle: no request -> no presents, no render-thread wake-ups, ~no CPU.
    pump_for(std::chrono::milliseconds{200});
    gpu::RenderLoop& loop = device->render_loop();
    const auto presents0 = surface->stats().frames_presented;
    const auto wakeups0 = loop.wakeups();
    const double cpu0 = loop.cpu_time_ms();
    pump_for(std::chrono::milliseconds{1000});
    const auto idle_presents = surface->stats().frames_presented - presents0;
    const auto idle_wakeups = loop.wakeups() - wakeups0;
    const double idle_cpu = loop.cpu_time_ms() - cpu0;
    std::cout << "  idle 1 s: " << idle_presents << " presents, " << idle_wakeups << " render-thread wake-ups, " << idle_cpu << " ms CPU\n";
    GPU_CHECK(idle_presents == 0);
    GPU_CHECK(idle_wakeups == 0);
    GPU_CHECK(idle_cpu < 1.0);

    // Animation: the callback requests the next frame; paced by the swap chain / compositor clock.
    const auto anim_start = Clock::now();
    const auto anim_before = surface->stats().frames_presented;
    const double anim_cpu0 = loop.cpu_time_ms();
    animate = 120;
    surface->request_frame();
    GPU_CHECK(pump_until([&]() { return animate.load() == 0; }, std::chrono::seconds{10}));
    const double anim_ms = ms_since(anim_start);
    const auto anim_frames = surface->stats().frames_presented - anim_before;
    report(std::string("animated frames/s (vsync paced) ") + std::string(label), static_cast<double>(anim_frames) * 1000.0 / anim_ms, "fps");
    report(std::string("render thread CPU per animated frame ") + std::string(label),
           (loop.cpu_time_ms() - anim_cpu0) / static_cast<double>(std::max<std::uint64_t>(1, anim_frames)), "ms");
    report(std::string("last frame callback+present CPU ") + std::string(label), surface->stats().last_frame_cpu_ms, "ms");
    GPU_CHECK(anim_frames >= 100);

    // Resize: buffers re-created at the new size (no stretching), readback at the new size.
    surface->set_bounds(RECT{20, 20, 320, 170});
    capture = true;
    GPU_CHECK(pump_until([&]() { return last_width.load() == 300; }));
    GPU_CHECK(pump_until([&]() { std::scoped_lock g(captured_mutex); return captured.has_value(); }));
    {
        std::scoped_lock guard(captured_mutex);
        if (captured) {
            GPU_CHECK(captured->width == 300 && captured->height == 150);
            GPU_CHECK(close_to(captured->at(299, 149), 0, 127, 255, 2));
        }
        captured.reset();
    }
    GPU_CHECK(surface->stats().resizes >= 1);
    GPU_CHECK(surface->stats().swap_chain_creations == 1);

    // Hidden surfaces and minimized windows do not render; they catch up when shown again.
    surface->set_visible(false);
    pump_for(std::chrono::milliseconds{50});
    const auto hidden0 = surface->stats().frames_presented;
    surface->request_frame();
    pump_for(std::chrono::milliseconds{150});
    GPU_CHECK(surface->stats().frames_presented == hidden0);
    surface->set_visible(true);
    GPU_CHECK(pump_until([&]() { return surface->stats().frames_presented > hidden0; }));

    window.minimize();
    pump_for(std::chrono::milliseconds{150});
    const auto min0 = surface->stats().frames_presented;
    surface->request_frame();
    pump_for(std::chrono::milliseconds{200});
    GPU_CHECK(surface->stats().frames_presented == min0);
    ShowWindow(window.hwnd(), SW_SHOWNOACTIVATE);
    GPU_CHECK(pump_until([&]() { return surface->stats().frames_presented > min0; }));

    surface.reset();
    pump_for(std::chrono::milliseconds{20});
    GPU_CHECK(host.surface_count() == 0);
}

void test_device_lost(const std::shared_ptr<gpu::Device>& device, gpu::SurfaceHost& host, std::string_view label) {
    step(std::string("device lost + recovery ") + std::string(label));
    std::atomic<int> lost_calls{0};
    std::atomic<bool> recovered{false};
    const auto listener = device->on_device_lost([&](const gpu::DeviceLostInfo& info) {
        lost_calls.fetch_add(1);
        recovered = info.recovered;
        std::cout << "  device lost: " << gpu::hresult_name(info.reason) << " recovered " << info.recovered << " generation "
                  << info.generation << '\n';
    });
    auto renderer = std::make_shared<gpu::Renderer2D>(device);
    auto atlas = std::make_shared<gpu::GlyphAtlas>(device, gpu::FontSpec{});
    std::atomic<bool> capture{false};
    std::optional<Image> captured;
    std::mutex captured_mutex;
    auto surface = host.create_surface(device, gpu::SurfaceOptions{.bounds = RECT{0, 0, 64, 64}});
    GPU_CHECK(surface != nullptr);
    if (!surface) {
        return;
    }
    surface->set_frame_callback([&, renderer, atlas](gpu::FrameContext& frame) {
        renderer->begin(frame, gpu::Color{0, 1, 0, 1});
        renderer->draw_text(*atlas, 2, 2, "ok", gpu::Color{1, 1, 1, 1});
        renderer->end();
        if (capture.exchange(false)) {
            auto image = read_texture(frame.device, frame.back_buffer);
            std::scoped_lock guard(captured_mutex);
            captured = std::move(image);
        }
    });
    surface->request_frame();
    GPU_CHECK(pump_until([&]() { return surface->stats().frames_presented >= 1; }));
    const std::uint64_t generation = device->generation();
    const auto start = Clock::now();
    GPU_CHECK(device->simulate_device_lost());
    GPU_CHECK(pump_until([&]() { return recovered.load(); }, std::chrono::seconds{15}));
    report(std::string("device-lost -> recovered ") + std::string(label), ms_since(start), "ms");
    GPU_CHECK(device->generation() == generation + 1);
    GPU_CHECK(device->removed_reason() == S_OK);
    capture = true;
    surface->request_frame();
    GPU_CHECK(pump_until([&]() { std::scoped_lock g(captured_mutex); return captured.has_value(); }, std::chrono::seconds{10}));
    {
        std::scoped_lock guard(captured_mutex);
        if (captured) {
            GPU_CHECK(close_to(captured->at(60, 60), 0, 255, 0));
        }
    }
    GPU_CHECK(surface->stats().swap_chain_creations == 2);
    GPU_CHECK(lost_calls.load() >= 1);
    device->off_device_lost(listener);
    surface.reset();
    pump_for(std::chrono::milliseconds{20});
}

void test_vulkan(const std::shared_ptr<gpu::Device>& device, std::string_view label) {
    step(std::string("vulkan interop ") + std::string(label));
    const gpu::VulkanSupport& support = gpu::vulkan_support();
    if (!support.loader) {
        std::cout << "  [skip] vulkan-1.dll not installed\n";
        return;
    }
    std::string why;
    const auto start = Clock::now();
    auto interop = gpu::VulkanInterop::create(device, &why);
    if (!interop) {
        std::cout << "  [skip] VulkanInterop unavailable: " << why << '\n';
        return;
    }
    report(std::string("VulkanInterop::create ") + std::string(label), ms_since(start), "ms");
    std::cout << "  Vulkan " << support.version_string() << " on '" << interop->device_name() << "', shared fence import "
              << interop->timeline_import_supported() << '\n';
    auto image = interop->create_image(64, 48);
    GPU_CHECK(image != nullptr);
    if (!image) {
        return;
    }
    std::cout << "  GPU-side sync: " << (image->fence_shared() ? "shared fence / timeline semaphore" : "CPU fallback") << '\n';
    double frame_ms = 0;
    constexpr int kFrames = 20;
    for (int i = 0; i < kFrames; ++i) {
        const auto frame_start = Clock::now();
        const float background[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        const float shade = static_cast<float>(i) / static_cast<float>(kFrames - 1);
        const gpu::VulkanClearRect rects[] = {
            {.x = 8, .y = 8, .width = 16, .height = 16, .rgba = {1.0f, 0.0f, 0.0f, 1.0f}},
            {.x = 40, .y = 30, .width = 10, .height = 10, .rgba = {shade, 1.0f, 0.0f, 1.0f}},
        };
        GPU_CHECK(image->begin_vulkan());
        image->clear_rects(background, rects);
        GPU_CHECK(image->end_vulkan());
        std::optional<Image> pixels;
        {
            const auto guard = device->lock();
            GPU_CHECK(image->acquire_d3d());
            pixels = read_texture(*device, image->texture());
            GPU_CHECK(image->release_d3d());
        }
        frame_ms += ms_since(frame_start);
        GPU_CHECK(pixels.has_value());
        if (pixels && (i == 0 || i == kFrames - 1)) {
            GPU_CHECK(close_to(pixels->at(2, 2), 0, 0, 255));
            GPU_CHECK(close_to(pixels->at(15, 15), 255, 0, 0));
            GPU_CHECK(close_to(pixels->at(45, 35), i == 0 ? 0 : 255, 255, 0));
            GPU_CHECK(close_to(pixels->at(63, 47), 0, 0, 255));
        }
    }
    report(std::string("Vulkan clear -> D3D readback round trip ") + std::string(label), frame_ms / kFrames, "ms/frame");

    // Through Renderer2D: the shared image sampled as a texture (the Surface presentation path).
    auto target = make_target(*device, 64, 48);
    GPU_CHECK(target.has_value());
    if (target) {
        gpu::Renderer2D renderer(device);
        const float background[4] = {1.0f, 1.0f, 0.0f, 1.0f};
        GPU_CHECK(image->begin_vulkan());
        image->clear_rects(background, {});
        GPU_CHECK(image->end_vulkan());
        {
            const auto guard = device->lock();
            GPU_CHECK(image->acquire_d3d());
            renderer.begin(target->rtv.Get(), 64, 48, gpu::Color{0, 0, 0, 1});
            renderer.draw_texture(image->srv(), {0, 0, 32, 48});
            renderer.end();
            GPU_CHECK(image->release_d3d());
        }
        const auto pixels = read_texture(*device, target->texture.Get());
        GPU_CHECK(pixels.has_value());
        if (pixels) {
            GPU_CHECK(close_to(pixels->at(10, 10), 255, 255, 0));
            GPU_CHECK(close_to(pixels->at(50, 10), 0, 0, 0));
        }
    }
    image.reset();
}

void test_controller(gpu::SurfaceHost& host, core::Window& window, const std::shared_ptr<gpu::Device>& device) {
    step("Gpu controller");
    boson::exec::UiMarshal marshal([&window]() { window.post_invoke(); });
    window.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    ipc::GpuController controller(host, marshal);
    controller.set_device(device);
    std::atomic<int> created{0};
    controller.register_content("solid", [&created](gpu::Surface& surface) {
        created.fetch_add(1);
        auto renderer = std::make_shared<gpu::Renderer2D>(surface.device());
        surface.set_frame_callback([renderer](gpu::FrameContext& frame) {
            renderer->begin(frame, gpu::Color{1, 0, 1, 1});
            renderer->end();
        });
    });
    ipc::Router<ipc::GpuController> router(controller);

    const auto call = [&](std::string request) {
        std::string response;
        std::atomic<bool> done{false};
        std::jthread worker([&]() {
            response = router.dispatch_sync(request);
            done = true;
        });
        pump_until([&]() { return done.load(); }, std::chrono::seconds{10});
        return response;
    };

    std::string response = call(R"({"id":1,"method":"Gpu.info","params":{}})");
    GPU_CHECK(response.find("\"status\":\"success\"") != std::string::npos);
    GPU_CHECK(response.find("\"contents\":[\"solid\"]") != std::string::npos);
    GPU_CHECK(response.find("\"hosting\":\"windowed\"") != std::string::npos);

    response = call(R"({"id":2,"method":"Gpu.create_surface","params":{"content":"solid","rect":{"x":10,"y":20,"width":50,"height":40},"scale":1.5,"page":"p1"}})");
    GPU_CHECK(response.find("\"status\":\"success\"") != std::string::npos);
    GPU_CHECK(response.find("\"width\":75") != std::string::npos);
    GPU_CHECK(host.surface_count() == 1);
    GPU_CHECK(created.load() == 1);
    const auto id_pos = response.find("\"id\":", response.find("\"data\""));
    const std::uint32_t id = id_pos == std::string::npos ? 0u : static_cast<std::uint32_t>(std::stoul(response.substr(id_pos + 5)));
    GPU_CHECK(id != 0);
    gpu::Surface* surface = controller.surface(id);
    GPU_CHECK(surface != nullptr);
    if (surface != nullptr) {
        GPU_CHECK(pump_until([&]() { return surface->stats().frames_presented >= 1; }));
        GPU_CHECK(surface->bounds().left == 15 && surface->bounds().top == 30 && surface->bounds().right == 90);
    }

    response = call(R"({"id":3,"method":"Gpu.set_bounds","params":{"scale":2,"surfaces":[{"id":)" + std::to_string(id) +
                    R"(,"rect":{"x":5,"y":5,"width":30,"height":20},"clip":{"x":0,"y":0,"width":30,"height":10},"visible":true}]}})");
    GPU_CHECK(response.find("\"ok\":true") != std::string::npos);
    pump_for(std::chrono::milliseconds{50});
    if (surface != nullptr) {
        GPU_CHECK(surface->bounds().left == 10 && surface->bounds().right == 70 && surface->bounds().bottom == 50);
        GPU_CHECK(pump_until([&]() { return surface->stats().resizes >= 1; }));
    }

    response = call(R"({"id":4,"method":"Gpu.create_surface","params":{"content":"nope","rect":{"x":0,"y":0,"width":5,"height":5}}})");
    GPU_CHECK(response.find("\"status\":\"error\"") != std::string::npos);

    // A new document token from the same sender replaces the surfaces of the previous document.
    response = call(R"({"id":5,"method":"Gpu.create_surface","params":{"content":"solid","rect":{"x":0,"y":0,"width":8,"height":8},"page":"p2"}})");
    GPU_CHECK(response.find("\"status\":\"success\"") != std::string::npos);
    GPU_CHECK(host.surface_count() == 1);
    GPU_CHECK(controller.surface(id) == nullptr);

    response = call(R"({"id":6,"method":"Gpu.destroy","params":{"id":)" + std::to_string(id + 1) + "}}");
    GPU_CHECK(response.find("\"ok\":true") != std::string::npos);
    GPU_CHECK(host.surface_count() == 0);
    window.set_invoke_callback(nullptr);
}

} // namespace

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    step("adapters");
    const auto adapters = gpu::Device::enumerate_adapters();
    for (const auto& a : adapters) {
        std::cout << "  " << a.name << " vendor 0x" << std::hex << a.vendor_id << std::dec << (a.software ? " (software)" : "") << '\n';
    }
    GPU_CHECK(!adapters.empty());

    auto d3d11 = test_device(gpu::Backend::D3D11, gpu::AdapterPreference::HighPerformance, "d3d11");
    auto d3d12 = test_device(gpu::Backend::D3D12, gpu::AdapterPreference::HighPerformance, "d3d12");
    auto warp = test_device(gpu::Backend::D3D11, gpu::AdapterPreference::Warp, "d3d11 warp");

    if (d3d11) {
        test_offscreen(d3d11, "d3d11");
        bench_text_grid(d3d11, "d3d11");
    }
    if (d3d12) {
        test_offscreen(d3d12, "d3d12");
        bench_text_grid(d3d12, "d3d12");
    }
    if (warp) {
        test_offscreen(warp, "warp");
    }

    {
        TestWindow windowed(L"Boson GPU test (windowed host)");
        GPU_CHECK(windowed.window.create(nullptr, SW_SHOWNOACTIVATE));
        gpu::SurfaceHost host(windowed.window);
        GPU_CHECK(!host.is_composition_hosted());
        if (d3d11) {
            test_surface(d3d11, host, windowed.window, "d3d11 windowed-host");
            test_controller(host, windowed.window, d3d11);
        }
        if (d3d12) {
            test_surface(d3d12, host, windowed.window, "d3d12 windowed-host");
        }

        TestWindow composed(L"Boson GPU test (composition host)");
        GPU_CHECK(composed.window.create(nullptr, SW_SHOWNOACTIVATE));
        auto composition = core::CompositionHost::create(composed.window.hwnd());
        GPU_CHECK(composition != nullptr);
        if (composition) {
            GPU_CHECK(composition->underlay() != nullptr && composition->overlay() != nullptr);
            gpu::SurfaceHost composed_host(composed.window, composition);
            GPU_CHECK(composed_host.is_composition_hosted());
            if (d3d11) {
                test_surface(d3d11, composed_host, composed.window, "d3d11 composition-host");
            }
        }

        if (d3d11) {
            test_vulkan(d3d11, "d3d11");
        }
        if (d3d12) {
            test_vulkan(d3d12, "d3d12");
        }
        if (d3d11) {
            test_device_lost(d3d11, host, "d3d11 (simulated)");
        }
        if (d3d12) {
            test_device_lost(d3d12, host, "d3d12 (RemoveDevice)");
        }
    }
    d3d11.reset();
    d3d12.reset();
    warp.reset();

    std::cout << (g_failures == 0 ? "[PASS] " : "[FAIL] ") << g_checks - g_failures << "/" << g_checks << " checks passed\n";
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return g_failures == 0 ? 0 : 1;
}
