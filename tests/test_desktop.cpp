// Desktop basics: clipboard, shell, global shortcuts, deep links, screen API, window state persistence.
// Every system change made here (clipboard contents, HKCU keys, Recycle Bin items, temp files) is undone
// before the process exits.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boson/boson.hpp>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_desktop.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define DT_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

void pump_once() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{5},
                boson::exec::UiMarshal* marshal = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
        pump_once();
        if (marshal != nullptr) {
            marshal->drain_on_ui_thread();
        }
    }
    return true;
}

void pump_for(std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    pump_until([until]() { return std::chrono::steady_clock::now() >= until; }, duration + std::chrono::seconds{1});
}

std::span<const std::byte> bytes_of(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::filesystem::path temp_dir() {
    wchar_t buffer[MAX_PATH + 1]{};
    GetTempPathW(MAX_PATH, buffer);
    auto dir = std::filesystem::path(buffer) / (L"boson-desktop-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    return dir;
}

// ---- Clipboard snapshot, so the user's clipboard survives the test run -----------------------------------

struct SavedFormat {
    UINT format = 0;
    std::vector<std::byte> data;
};

bool open_clipboard_retry() {
    for (int i = 0; i < 200; ++i) {
        if (OpenClipboard(nullptr)) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

std::vector<SavedFormat> snapshot_clipboard() {
    std::vector<SavedFormat> saved;
    if (!open_clipboard_retry()) {
        return saved;
    }
    UINT format = 0;
    while ((format = EnumClipboardFormats(format)) != 0) {
        const bool gdi = format == CF_BITMAP || format == CF_ENHMETAFILE || format == CF_METAFILEPICT || format == CF_PALETTE ||
                         format == CF_DSPBITMAP || format == CF_DSPENHMETAFILE || format == CF_DSPMETAFILEPICT ||
                         format == CF_OWNERDISPLAY || (format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST);
        if (gdi) {
            continue; // CF_BITMAP is re-synthesised from CF_DIB / CF_DIBV5
        }
        HANDLE handle = GetClipboardData(format);
        if (handle == nullptr) {
            continue;
        }
        const SIZE_T size = GlobalSize(handle);
        const void* src = GlobalLock(handle);
        if (src == nullptr) {
            continue;
        }
        SavedFormat entry{.format = format, .data = std::vector<std::byte>(size)};
        std::memcpy(entry.data.data(), src, size);
        GlobalUnlock(handle);
        saved.push_back(std::move(entry));
    }
    CloseClipboard();
    return saved;
}

void restore_clipboard(const std::vector<SavedFormat>& saved) {
    if (!open_clipboard_retry()) {
        std::cerr << "warning: could not reopen the clipboard to restore it\n";
        return;
    }
    EmptyClipboard();
    for (const auto& entry : saved) {
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, entry.data.empty() ? 1 : entry.data.size());
        if (global == nullptr) {
            continue;
        }
        if (void* dst = GlobalLock(global); dst != nullptr) {
            if (!entry.data.empty()) {
                std::memcpy(dst, entry.data.data(), entry.data.size());
            }
            GlobalUnlock(global);
        }
        if (SetClipboardData(entry.format, global) == nullptr) {
            GlobalFree(global);
        }
    }
    CloseClipboard();
}

// ---- Recycle Bin cleanup ---------------------------------------------------------------------------------

int purge_from_recycle_bin_once(std::wstring_view name_prefix) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IShellItem> bin;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin)))) {
        return -1;
    }
    ComPtr<IEnumShellItems> items;
    if (FAILED(bin->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) {
        return -1;
    }
    ComPtr<IFileOperation> operation;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation)))) {
        return -1;
    }
    operation->SetOperationFlags(FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI);
    int found = 0;
    ComPtr<IShellItem> item;
    while (items->Next(1, &item, nullptr) == S_OK) {
        bool match = false;
        for (const SIGDN form : {SIGDN_NORMALDISPLAY, SIGDN_PARENTRELATIVEEDITING, SIGDN_PARENTRELATIVE}) {
            PWSTR name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(form, &name)) && name != nullptr) {
                match = match || std::wstring_view{name}.starts_with(name_prefix);
                CoTaskMemFree(name);
            }
        }
        if (match) {
            operation->DeleteItem(item.Get(), nullptr);
            ++found;
        }
        item.Reset();
    }
    if (found > 0 && FAILED(operation->PerformOperations())) {
        return -1;
    }
    return found;
}

/// The Recycle Bin view can lag behind the delete operation by a moment.
int purge_from_recycle_bin(std::wstring_view name_prefix) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        const int found = purge_from_recycle_bin_once(name_prefix);
        if (found != 0) {
            return found;
        }
        Sleep(100);
    }
    return 0;
}

std::wstring read_hkcu_default(const std::wstring& sub_key) {
    wchar_t buffer[2048]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, sub_key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return {};
    }
    return buffer;
}

bool hkcu_key_exists(const std::wstring& sub_key) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub_key.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}

boson::desktop::BgraImage sample_image() {
    boson::desktop::BgraImage image;
    image.width = 3;
    image.height = 2;
    const unsigned char px[] = {
        0x00, 0x00, 0xFF, 0xFF,  0x00, 0xFF, 0x00, 0x80,  0xFF, 0x00, 0x00, 0x00,
        0x10, 0x20, 0x30, 0xFF,  0xFF, 0xFF, 0xFF, 0x40,  0x01, 0x02, 0x03, 0xC0,
    };
    for (const unsigned char b : px) {
        image.pixels.push_back(static_cast<std::byte>(b));
    }
    return image;
}

std::string json_for(std::string_view method, std::string_view params = "{}") {
    return std::string(R"({"id":7,"method":")") + std::string(method) + R"(","params":)" + std::string(params) + "}";
}

bool is_success(const std::string& envelope) {
    return envelope.find(R"("status":"success")") != std::string::npos;
}

} // namespace

// ==== Pure helpers ========================================================================================

void test_cf_html() {
    step("CF_HTML build / parse");
    using namespace boson::desktop;
    const std::string fragment = "<b>Ol\xC3\xA1</b> <i>\xE6\x97\xA5\xE6\x9C\xAC</i>"; // UTF-8: offsets are bytes
    const std::string cf = build_cf_html(fragment, "https://example.com/page");

    const auto number = [&cf](std::string_view key) -> std::size_t {
        const std::size_t at = cf.find(std::string(key) + ":");
        return at == std::string::npos ? std::string::npos : std::stoul(cf.substr(at + key.size() + 1, 10));
    };
    const std::size_t sh = number("StartHTML");
    const std::size_t eh = number("EndHTML");
    const std::size_t sf = number("StartFragment");
    const std::size_t ef = number("EndFragment");
    DT_CHECK(cf.starts_with("Version:0.9\r\n"));
    DT_CHECK(eh == cf.size());
    DT_CHECK(cf.substr(sf, ef - sf) == fragment);
    DT_CHECK(cf.substr(sh, 6) == "<html>");
    DT_CHECK(cf.substr(eh - 7, 7) == "</html>");
    DT_CHECK(cf.substr(sf - 20, 20) == "<!--StartFragment-->");
    DT_CHECK(cf.find("SourceURL:https://example.com/page\r\n") != std::string::npos);
    DT_CHECK(cf.find('<') == sh);

    const auto parsed = parse_cf_html(cf + '\0');
    DT_CHECK(parsed.has_value());
    if (parsed) {
        DT_CHECK(parsed->fragment == fragment);
        DT_CHECK(parsed->source_url == "https://example.com/page");
        DT_CHECK(parsed->html.starts_with("<html>") && parsed->html.ends_with("</html>"));
    }

    // Broken offsets: fall back to the fragment markers.
    const std::string broken = "Version:1.0\r\nStartHTML:-1\r\nEndHTML:-1\r\nStartFragment:999999\r\nEndFragment:999999\r\n"
                               "<html><body><!--StartFragment-->x<br>y<!--EndFragment--></body></html>";
    const auto fallback = parse_cf_html(broken);
    DT_CHECK(fallback.has_value() && fallback->fragment == "x<br>y");
    DT_CHECK(!parse_cf_html("").has_value());
    DT_CHECK(!parse_cf_html("no header here").has_value());
    // Newlines in a SourceURL would corrupt the header.
    DT_CHECK(build_cf_html("a", "http://x/\r\nEvil:1").find("\r\nEvil:1") == std::string::npos);
}

void test_base64() {
    step("base64");
    using namespace boson::desktop;
    const std::pair<std::string_view, std::string_view> vectors[] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
    };
    for (const auto& [plain, encoded] : vectors) {
        DT_CHECK(base64_encode(bytes_of(plain)) == encoded);
        const auto decoded = base64_decode(encoded);
        DT_CHECK(decoded.has_value() && std::string(reinterpret_cast<const char*>(decoded->data()), decoded->size()) == plain);
    }
    DT_CHECK(base64_decode("Zm9v\r\nYmFy").has_value());
    DT_CHECK(base64_decode("Zm9vYg").has_value()); // missing padding
    DT_CHECK(!base64_decode("Zm9v!").has_value());
    DT_CHECK(!base64_decode("Z").has_value());
    DT_CHECK(!base64_decode("Zg==Zg").has_value());
    const auto data_url = base64_decode("data:image/png;base64,Zm9v");
    DT_CHECK(data_url.has_value() && data_url->size() == 3);
}

void test_images() {
    step("PNG <-> CF_DIBV5 via WIC");
    using namespace boson::desktop;
    const BgraImage image = sample_image();
    const auto png = encode_png(image);
    DT_CHECK(png.has_value());
    if (!png) {
        return;
    }
    const auto dims = png_dimensions(*png);
    DT_CHECK(dims.has_value() && dims->first == 3 && dims->second == 2);
    const auto decoded = decode_png(*png);
    DT_CHECK(decoded.has_value() && decoded->pixels == image.pixels);

    const auto dib = png_to_dibv5(*png);
    DT_CHECK(dib.has_value());
    if (dib) {
        BITMAPV5HEADER header{};
        std::memcpy(&header, dib->data(), sizeof(header));
        DT_CHECK(header.bV5Size == sizeof(BITMAPV5HEADER));
        DT_CHECK(header.bV5Width == 3 && header.bV5Height == 2);
        DT_CHECK(header.bV5BitCount == 32 && header.bV5Compression == BI_BITFIELDS);
        DT_CHECK(header.bV5AlphaMask == 0xFF000000u);
        DT_CHECK(dib->size() == sizeof(BITMAPV5HEADER) + 3 * 2 * 4);
        // Bottom-up: the first stored row is the last image row.
        DT_CHECK(std::memcmp(dib->data() + sizeof(BITMAPV5HEADER), image.pixels.data() + 12, 12) == 0);

        const auto back = dib_to_png(*dib);
        DT_CHECK(back.has_value());
        if (back) {
            const auto round = decode_png(*back);
            DT_CHECK(round.has_value() && round->pixels == image.pixels); // alpha preserved
        }
    }

    // 24-bit BI_RGB CF_DIB, 2x1, bottom-up, rows padded to 4 bytes.
    {
        std::vector<std::byte> dib24(sizeof(BITMAPINFOHEADER) + 8);
        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof(bih);
        bih.biWidth = 2;
        bih.biHeight = 1;
        bih.biPlanes = 1;
        bih.biBitCount = 24;
        bih.biCompression = BI_RGB;
        std::memcpy(dib24.data(), &bih, sizeof(bih));
        const unsigned char row[] = {0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0, 0}; // blue, red, padding
        std::memcpy(dib24.data() + sizeof(bih), row, sizeof(row));
        const auto png24 = dib_to_png(dib24);
        DT_CHECK(png24.has_value());
        if (png24) {
            const auto img = decode_png(*png24);
            DT_CHECK(img.has_value() && img->width == 2 && img->height == 1);
            if (img && img->pixels.size() == 8) {
                DT_CHECK(std::to_integer<int>(img->pixels[0]) == 0xFF && std::to_integer<int>(img->pixels[2]) == 0x00);
                DT_CHECK(std::to_integer<int>(img->pixels[6]) == 0xFF && std::to_integer<int>(img->pixels[3]) == 0xFF);
            }
        }
    }
    // 32-bit BI_RGB with an all-zero reserved byte is opaque, not transparent.
    {
        std::vector<std::byte> dib32(sizeof(BITMAPINFOHEADER) + 4);
        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof(bih);
        bih.biWidth = 1;
        bih.biHeight = -1;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        std::memcpy(dib32.data(), &bih, sizeof(bih));
        const unsigned char px[] = {0x11, 0x22, 0x33, 0x00};
        std::memcpy(dib32.data() + sizeof(bih), px, sizeof(px));
        const auto png32 = dib_to_png(dib32);
        const auto img = png32 ? decode_png(*png32) : std::nullopt;
        DT_CHECK(img.has_value() && std::to_integer<int>(img->pixels[3]) == 0xFF && std::to_integer<int>(img->pixels[0]) == 0x11);
    }
    DT_CHECK(!decode_png(bytes_of("not a png")).has_value());
    DT_CHECK(!dib_to_png(bytes_of("tiny")).has_value());
    DT_CHECK(!png_to_dibv5(bytes_of("not a png")).has_value());
}

// ==== System clipboard ====================================================================================

/// Another process (clipboard history, cross-device sync, clipboard managers) may empty and re-post the
/// clipboard right after every write. Wait until the sequence number has been stable for 150 ms before
/// reading back, so the round-trip checks test Boson and not the timing of those processes.
void settle_clipboard() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    DWORD seq = GetClipboardSequenceNumber();
    auto stable_since = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        Sleep(10);
        const DWORD now_seq = GetClipboardSequenceNumber();
        if (now_seq != seq) {
            seq = now_seq;
            stable_since = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - stable_since >= std::chrono::milliseconds{150}) {
            return;
        }
    }
}

void test_clipboard() {
    step("system clipboard (contents saved and restored)");
    using namespace boson::desktop;
    const auto saved = snapshot_clipboard();

    int notifications = 0;
    std::uint32_t last_seq = 0;
    ClipboardWatcher watcher([&](const ClipboardChange& c) {
        ++notifications;
        last_seq = c.sequence_number;
    });
    DT_CHECK(watcher.is_active());

    const Clipboard clipboard;
    const std::string text = "Boson \xE2\x9C\x93 clipboard \xF0\x9F\x9A\x80";
    DT_CHECK(clipboard.write_text(text).has_value());
    settle_clipboard();
    const auto read = clipboard.read_text();
    DT_CHECK(read.has_value() && read->has_value() && **read == text);
    DT_CHECK(Clipboard::has_format("text") && Clipboard::has_format("CF_UNICODETEXT"));
    DT_CHECK(pump_until([&]() { return notifications > 0; }));
    DT_CHECK(last_seq != 0);

    const std::string html = "<p>Hello <b>world</b> \xC3\xA9</p>";
    DT_CHECK(clipboard.write_html(html, std::string_view{"Hello world"}).has_value());
    settle_clipboard();
    const auto read_html = clipboard.read_html();
    DT_CHECK(read_html.has_value() && read_html->has_value() && (*read_html)->fragment == html);
    const auto alt = clipboard.read_text();
    DT_CHECK(alt.has_value() && alt->has_value() && **alt == "Hello world");
    const auto formats = clipboard.formats();
    DT_CHECK(formats.has_value());
    if (formats) {
        bool has_html = false;
        bool has_unicode = false;
        for (const auto& f : *formats) {
            has_html |= f.name == "HTML Format";
            has_unicode |= f.name == "CF_UNICODETEXT";
        }
        DT_CHECK(has_html && has_unicode);
    }

    const auto png = encode_png(sample_image());
    DT_CHECK(png.has_value());
    if (png) {
        DT_CHECK(clipboard.write_image_png(*png).has_value());
        settle_clipboard();
        DT_CHECK(Clipboard::has_format("image") && Clipboard::has_format("CF_DIBV5") && Clipboard::has_format("PNG"));
        const auto image = clipboard.read_image_png();
        DT_CHECK(image.has_value() && image->has_value());
        if (image && *image) {
            const auto decoded = decode_png(**image);
            DT_CHECK(decoded.has_value() && decoded->pixels == sample_image().pixels);
        }
        // DIB-only path: what a non-PNG-aware app (Paint, screenshots) leaves on the clipboard.
        if (const auto dib = png_to_dibv5(*png); dib && open_clipboard_retry()) {
            EmptyClipboard();
            HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, dib->size());
            std::memcpy(GlobalLock(global), dib->data(), dib->size());
            GlobalUnlock(global);
            if (SetClipboardData(CF_DIBV5, global) == nullptr) {
                GlobalFree(global);
            }
            CloseClipboard();
            settle_clipboard();
            const auto from_dib = clipboard.read_image_png();
            DT_CHECK(from_dib.has_value() && from_dib->has_value());
            if (from_dib && *from_dib) {
                const auto decoded = decode_png(**from_dib);
                DT_CHECK(decoded.has_value() && decoded->pixels == sample_image().pixels);
            }
        }
    }

    const auto dir = temp_dir();
    const std::vector<std::filesystem::path> files{dir / L"a file.txt", dir / L"\x00E9t\x00E9.bin"};
    for (const auto& f : files) {
        std::ofstream(f) << "x";
    }
    DT_CHECK(clipboard.write_files(files).has_value());
    settle_clipboard();
    const auto read_files = clipboard.read_files();
    DT_CHECK(read_files.has_value() && read_files->has_value() && (*read_files)->size() == 2);
    if (read_files && *read_files && (*read_files)->size() == 2) {
        DT_CHECK((**read_files)[0] == files[0] && (**read_files)[1] == files[1]);
    }
    DT_CHECK(Clipboard::has_format("files") && Clipboard::has_format("Preferred DropEffect"));

    DT_CHECK(clipboard.write_text("secret", {.exclude_from_history = true}).has_value());
    settle_clipboard();
    DT_CHECK(Clipboard::has_format("ExcludeClipboardContentFromMonitorProcessing"));
    DT_CHECK(Clipboard::has_format("CanIncludeInClipboardHistory"));

    DT_CHECK(clipboard.clear().has_value());
    settle_clipboard();
    const auto empty = clipboard.read_text();
    DT_CHECK(empty.has_value() && !empty->has_value());
    const auto no_image = clipboard.read_image_png();
    DT_CHECK(no_image.has_value() && !no_image->has_value());

    DT_CHECK(Clipboard::format_name(CF_UNICODETEXT) == "CF_UNICODETEXT");
    DT_CHECK(Clipboard::format_id("cf_dibv5") == CF_DIBV5);
    DT_CHECK(Clipboard::format_name(Clipboard::format_id("HTML Format")) == "HTML Format");
    DT_CHECK(Clipboard::format_id("") == 0);

    // Controller round trip through the router (same JSON the frontend sends).
    boson::ipc::ClipboardController controller;
    boson::ipc::Router<boson::ipc::ClipboardController> router(controller);
    DT_CHECK(is_success(router.dispatch_sync(json_for("Clipboard.write_text", R"({"text":"via rpc"})"))));
    settle_clipboard();
    const auto rpc_read = router.dispatch_sync(json_for("Clipboard.read_text"));
    DT_CHECK(rpc_read.find(R"("text":"via rpc")") != std::string::npos);
    const auto rpc_has = router.dispatch_sync(json_for("Clipboard.has", R"({"format":"text"})"));
    DT_CHECK(rpc_has.find(R"("value":true)") != std::string::npos);
    if (png) {
        const std::string params = R"({"png_base64":")" + base64_encode(*png) + R"("})";
        DT_CHECK(is_success(router.dispatch_sync(json_for("Clipboard.write_image", params))));
        settle_clipboard();
        const auto rpc_image = router.dispatch_sync(json_for("Clipboard.read_image"));
        DT_CHECK(rpc_image.find(R"("available":true)") != std::string::npos && rpc_image.find(R"("width":3)") != std::string::npos);
    }
    const auto bad_image = router.dispatch_sync(json_for("Clipboard.write_image", R"({"png_base64":"!!"})"));
    DT_CHECK(bad_image.find(R"("status":"error")") != std::string::npos);
    DT_CHECK(is_success(router.dispatch_sync(json_for("Clipboard.formats"))));

    restore_clipboard(saved);
    pump_for(std::chrono::milliseconds{50});
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ==== Shell ===============================================================================================

void test_shell() {
    step("shell: URL validation, known folders, trash");
    using namespace boson::desktop;
    DT_CHECK(url_scheme("HTTPS://example.com") == std::optional<std::string>{"https"});
    DT_CHECK(url_scheme("mailto:a@b.c") == std::optional<std::string>{"mailto"});
    DT_CHECK(!url_scheme("C:\\Windows").has_value());
    DT_CHECK(!url_scheme("c:/x").has_value());
    DT_CHECK(!url_scheme("no-scheme").has_value());
    DT_CHECK(!url_scheme("1abc:x").has_value());

    DT_CHECK(validate_external_url("https://example.com/a?b=c#d").has_value());
    DT_CHECK(validate_external_url("http://localhost:8080").has_value());
    DT_CHECK(validate_external_url("mailto:someone@example.com").has_value());
    DT_CHECK(!validate_external_url("file:///C:/Windows/System32/calc.exe").has_value());
    DT_CHECK(!validate_external_url("ms-settings:display").has_value());
    DT_CHECK(!validate_external_url("javascript:alert(1)").has_value());
    DT_CHECK(!validate_external_url("C:\\Windows\\notepad.exe").has_value());
    DT_CHECK(!validate_external_url("https://").has_value());
    DT_CHECK(!validate_external_url("https:///path").has_value());
    DT_CHECK(!validate_external_url("https://a.com/ b").has_value());
    DT_CHECK(!validate_external_url("https://a.com/\"x").has_value());
    DT_CHECK(!validate_external_url("").has_value());
    DT_CHECK(validate_external_url("vscode://file/x", {.allowed_schemes = {"vscode"}}).has_value());
    DT_CHECK(!open_external("file:///C:/").has_value()); // rejected before anything launches

    for (const auto folder : {KnownFolder::Home, KnownFolder::Desktop, KnownFolder::Documents, KnownFolder::Downloads,
                              KnownFolder::AppData, KnownFolder::LocalAppData, KnownFolder::Temp, KnownFolder::Executable,
                              KnownFolder::ProgramFiles, KnownFolder::Fonts}) {
        const auto path = known_folder(folder);
        DT_CHECK(path.has_value() && !path->empty() && std::filesystem::exists(*path));
        DT_CHECK(parse_known_folder(known_folder_name(folder)) == std::optional<KnownFolder>{folder});
    }
    DT_CHECK(parse_known_folder("LocalAppData") == std::optional<KnownFolder>{KnownFolder::LocalAppData});
    DT_CHECK(parse_known_folder("local_app_data") == std::optional<KnownFolder>{KnownFolder::LocalAppData});
    DT_CHECK(!parse_known_folder("nope").has_value());

    const auto dir = temp_dir();
    DT_CHECK(!open_path(dir / L"does-not-exist.txt").has_value());
    DT_CHECK(!show_item_in_folder(dir / L"does-not-exist.txt").has_value());
    DT_CHECK(!move_to_trash(dir / L"does-not-exist.txt").has_value());

    const std::wstring unique = L"boson-trash-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto victim = dir / unique;
    std::ofstream(victim) << "trash me";
    const auto trashed = move_to_trash(victim);
    if (!trashed) {
        std::cerr << "  move_to_trash: " << trashed.error() << '\n';
    }
    DT_CHECK(trashed.has_value());
    DT_CHECK(!std::filesystem::exists(victim));
    const int purged = purge_from_recycle_bin(unique);
    std::cout << "  purged " << purged << " test item(s) from the Recycle Bin\n";
    DT_CHECK(purged == 1);

    boson::ipc::ShellController controller;
    boson::ipc::Router<boson::ipc::ShellController> router(controller);
    const auto docs = router.dispatch_sync(json_for("Shell.get_known_folder", R"({"name":"documents"})"));
    DT_CHECK(is_success(docs) && docs.find(R"("path":")") != std::string::npos);
    DT_CHECK(!is_success(router.dispatch_sync(json_for("Shell.get_known_folder", R"({"name":"bogus"})"))));
    DT_CHECK(!is_success(router.dispatch_sync(json_for("Shell.open_external", R"({"url":"file:///C:/"})"))));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ==== Global shortcuts ====================================================================================

void test_shortcuts() {
    step("global shortcuts");
    using namespace boson::desktop;
    const auto a = parse_accelerator("CommandOrControl+Shift+K");
    DT_CHECK(a.has_value() && a->modifiers == (MOD_CONTROL | MOD_SHIFT) && a->key == 'K' && a->canonical == "Ctrl+Shift+K");
    const auto b = parse_accelerator("shift+cmdorctrl+k");
    DT_CHECK(b.has_value() && a.has_value() && *a == *b);
    const auto f = parse_accelerator("Alt+F24");
    DT_CHECK(f.has_value() && f->key == VK_F24 && f->modifiers == MOD_ALT && f->canonical == "Alt+F24");
    const auto super = parse_accelerator("Super+Space");
    DT_CHECK(super.has_value() && super->modifiers == MOD_WIN && super->key == VK_SPACE && super->canonical == "Super+Space");
    const auto media = parse_accelerator("MediaPlayPause");
    DT_CHECK(media.has_value() && media->modifiers == 0 && media->key == VK_MEDIA_PLAY_PAUSE);
    const auto num = parse_accelerator("Ctrl+num5");
    DT_CHECK(num.has_value() && num->key == VK_NUMPAD5 && num->canonical == "Ctrl+Num5");
    const auto altgr = parse_accelerator("AltGr+E");
    DT_CHECK(altgr.has_value() && altgr->modifiers == (MOD_CONTROL | MOD_ALT));
    const auto plus = parse_accelerator("Ctrl+Plus");
    const auto plus2 = parse_accelerator("Ctrl++");
    DT_CHECK(plus.has_value() && plus2.has_value() && *plus == *plus2);
    if (plus) {
        const auto again = parse_accelerator(plus->canonical);
        DT_CHECK(again.has_value() && *again == *plus); // canonical form round-trips
    }
    DT_CHECK(parse_accelerator(" Ctrl + Enter ").has_value());
    DT_CHECK(parse_accelerator("Ctrl+Return")->key == VK_RETURN);
    DT_CHECK(parse_accelerator("Esc")->key == VK_ESCAPE);
    DT_CHECK(!parse_accelerator("").has_value());
    DT_CHECK(!parse_accelerator("Ctrl+Shift").has_value());
    DT_CHECK(!parse_accelerator("Ctrl+K+L").has_value());
    DT_CHECK(!parse_accelerator("K+Ctrl").has_value());
    DT_CHECK(!parse_accelerator("Ctrl+F25").has_value());
    DT_CHECK(!parse_accelerator("Ctrl+Bogus").has_value());
    DT_CHECK(!parse_accelerator("Ctrl++K").has_value());
    DT_CHECK(virtual_key_name(VK_F5) == "F5" && virtual_key_name('7') == "7");

    // F21..F24 with every modifier: no keyboard has them, so nothing on the machine is disturbed.
    const std::string accel = "Ctrl+Alt+Shift+F23";
    GlobalShortcuts shortcuts;
    DT_CHECK(shortcuts.hwnd() != nullptr);
    int fired = 0;
    std::string fired_accel;
    const auto id = shortcuts.add(accel, [&](const ShortcutEvent& e) {
        ++fired;
        fired_accel = e.accelerator;
    });
    if (!id) {
        std::cerr << "  register failed: " << id.error().message << '\n';
    }
    DT_CHECK(id.has_value());
    if (id) {
        DT_CHECK(shortcuts.is_registered("shift+alt+ctrl+f23"));
        const auto dup = shortcuts.add("CmdOrCtrl+Alt+Shift+F23", {});
        DT_CHECK(!dup.has_value() && dup.error().kind == ShortcutErrorKind::AlreadyRegistered);

        GlobalShortcuts other;
        const auto conflict = other.add(accel, {});
        DT_CHECK(!conflict.has_value() && conflict.error().kind == ShortcutErrorKind::Conflict);

        // WM_HOTKEY is what the system posts on a key press; posting it exercises the dispatch path.
        PostMessageW(shortcuts.hwnd(), WM_HOTKEY, static_cast<WPARAM>(*id), 0);
        DT_CHECK(pump_until([&]() { return fired == 1; }));
        DT_CHECK(fired_accel == "Ctrl+Alt+Shift+F23");
        PostMessageW(shortcuts.hwnd(), WM_HOTKEY, static_cast<WPARAM>(*id + 1000), 0); // unknown id: ignored
        pump_for(std::chrono::milliseconds{30});
        DT_CHECK(fired == 1);

        DT_CHECK(shortcuts.list().size() == 1);
        DT_CHECK(shortcuts.remove(*id));
        DT_CHECK(!shortcuts.remove(*id));
        DT_CHECK(!shortcuts.is_registered(accel));
        const auto after = other.add(accel, {}); // released: another registry may now take it
        DT_CHECK(after.has_value());
        other.remove_all();
    }
    const auto invalid = shortcuts.add("Ctrl+", {});
    DT_CHECK(!invalid.has_value() && invalid.error().kind == ShortcutErrorKind::InvalidAccelerator);

    // Controller: called from a pool thread, marshalled to this (UI) thread.
    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([]() {});
    boson::exec::TaskRunner runner(scheduler, marshal);
    boson::ipc::Bridge bridge(runner, marshal, [](std::string_view, boson::ipc::Bridge::ReplyFn) {});
    boson::ipc::ShortcutController controller(shortcuts, marshal, bridge);
    boson::ipc::Router<boson::ipc::ShortcutController> router(controller);
    std::string reg;
    std::string dup;
    std::string listed;
    std::string unreg;
    bool done = false;
    runner.spawn([&]() {
        reg = router.dispatch_sync(json_for("Shortcut.register", R"({"accelerator":"CommandOrControl+Alt+Shift+F22"})"));
        dup = router.dispatch_sync(json_for("Shortcut.register", R"({"accelerator":"Ctrl+Shift+Alt+F22"})"));
        listed = router.dispatch_sync(json_for("Shortcut.list"));
        unreg = router.dispatch_sync(json_for("Shortcut.unregister_all"));
        marshal.post_task([&done]() { done = true; });
    });
    DT_CHECK(pump_until([&]() { return done; }, std::chrono::seconds{10}, &marshal));
    DT_CHECK(reg.find(R"("ok":true)") != std::string::npos && reg.find(R"("accelerator":"Ctrl+Alt+Shift+F22")") != std::string::npos);
    DT_CHECK(dup.find(R"("error":"already_registered")") != std::string::npos);
    DT_CHECK(listed.find("F22") != std::string::npos);
    DT_CHECK(is_success(unreg));
    DT_CHECK(shortcuts.size() == 0);

    // Emit-callback form: a press of a JS-registered shortcut produces a ShortcutTriggeredEvent.
    std::vector<boson::ipc::ShortcutTriggeredEvent> emitted;
    boson::ipc::ShortcutController emitting(shortcuts, marshal, [&emitted](const boson::ipc::ShortcutTriggeredEvent& e) {
        emitted.push_back(e);
    });
    boson::ipc::Router<boson::ipc::ShortcutController> emitting_router(emitting);
    std::string reg2;
    bool done2 = false;
    runner.spawn([&]() {
        reg2 = emitting_router.dispatch_sync(json_for("Shortcut.register", R"({"accelerator":"CmdOrCtrl+Alt+Shift+F21"})"));
        marshal.post_task([&done2]() { done2 = true; });
    });
    DT_CHECK(pump_until([&]() { return done2; }, std::chrono::seconds{10}, &marshal));
    DT_CHECK(reg2.find(R"("ok":true)") != std::string::npos);
    if (reg2.find(R"("ok":true)") == std::string::npos) {
        std::cerr << "  " << reg2 << '\n';
    }
    if (const auto jsid = shortcuts.find("Ctrl+Alt+Shift+F21")) {
        PostMessageW(shortcuts.hwnd(), WM_HOTKEY, static_cast<WPARAM>(*jsid), 0);
        DT_CHECK(pump_until([&]() { return !emitted.empty(); }));
        DT_CHECK(emitted.size() == 1 && emitted[0].id == *jsid && emitted[0].accelerator == "Ctrl+Alt+Shift+F21");
    } else {
        DT_CHECK(false);
    }
    shortcuts.remove_all();
}

// ==== Deep links ==========================================================================================

void test_deep_links() {
    step("deep links (HKCU registration, removed afterwards)");
    using namespace boson::desktop;
    DT_CHECK(is_valid_custom_scheme("myapp"));
    DT_CHECK(is_valid_custom_scheme("my-app.v2+x"));
    DT_CHECK(!is_valid_custom_scheme("c"));
    DT_CHECK(!is_valid_custom_scheme("http"));
    DT_CHECK(!is_valid_custom_scheme("HTTPS"));
    DT_CHECK(!is_valid_custom_scheme("ms-settings"));
    DT_CHECK(!is_valid_custom_scheme("2fa"));
    DT_CHECK(!is_valid_custom_scheme("my app"));

    DT_CHECK(command_program(L"\"C:\\Program Files\\A\\a.exe\" \"%1\"") == L"C:\\Program Files\\A\\a.exe");
    DT_CHECK(command_program(L"C:\\a.exe %1") == L"C:\\a.exe");

    const std::vector<std::wstring> args{L"--flag", L"MyApp://open?x=1", L"other://x", L"myapp:", L"myapp:/path"};
    const auto links = extract_deep_links(args, "myapp");
    DT_CHECK(links.size() == 2 && links[0] == "MyApp://open?x=1" && links[1] == "myapp:/path");

    std::vector<std::string> forwarded;
    int other_launches = 0;
    const auto forwarder = deep_link_forwarder(
        "myapp", [&](std::vector<std::string> l) { forwarded = std::move(l); },
        [&](std::vector<std::wstring>, std::wstring) { ++other_launches; });
    forwarder({L"myapp://a"}, L"C:\\");
    forwarder({L"--no-link"}, L"C:\\");
    DT_CHECK(forwarded.size() == 1 && forwarded[0] == "myapp://a" && other_launches == 1);

    const std::string scheme = "boson-test-" + std::to_string(GetCurrentProcessId());
    const std::wstring key = L"Software\\Classes\\" + boson::core::utf8_to_wide(scheme);
    DT_CHECK(!is_default_protocol_client(scheme));
    const auto reg = register_protocol_client(scheme, {.arguments = {L"--deep-link"}, .description = L"Boson Test"});
    if (!reg) {
        std::cerr << "  register_protocol_client: " << reg.error() << '\n';
    }
    DT_CHECK(reg.has_value());
    const std::wstring exe = boson::app::executable_path();
    DT_CHECK(read_hkcu_default(key) == L"URL:Boson Test");
    DT_CHECK(read_hkcu_default(key + L"\\shell\\open\\command") == boson::app::quote_argument(exe) + L" --deep-link \"%1\"");
    DT_CHECK(hkcu_key_exists(key + L"\\DefaultIcon"));
    DT_CHECK(is_default_protocol_client(scheme));
    DT_CHECK(!is_default_protocol_client(scheme, L"C:\\Windows\\notepad.exe"));
    DT_CHECK(!protocol_handler_command(scheme).empty());

    const auto wrong = unregister_protocol_client(scheme, L"C:\\Windows\\notepad.exe");
    DT_CHECK(!wrong.has_value() && hkcu_key_exists(key)); // someone else's registration is left alone
    DT_CHECK(unregister_protocol_client(scheme).has_value());
    DT_CHECK(!hkcu_key_exists(key));
    DT_CHECK(!is_default_protocol_client(scheme));
    DT_CHECK(unregister_protocol_client(scheme).has_value()); // idempotent
    DT_CHECK(!register_protocol_client("https").has_value());
    if (hkcu_key_exists(key)) {
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
    }
}

// ==== Screen ==============================================================================================

void test_screen() {
    step("screen API");
    using namespace boson::desktop;
    const auto all = displays();
    DT_CHECK(!all.empty());
    int primaries = 0;
    const ScreenRect virt = virtual_screen();
    for (const auto& d : all) {
        primaries += d.primary ? 1 : 0;
        DT_CHECK(!d.id.empty() && d.id == d.device_name);
        DT_CHECK(d.bounds.width > 0 && d.bounds.height > 0);
        DT_CHECK(d.work_area.width > 0 && d.work_area.width <= d.bounds.width);
        DT_CHECK(d.work_area.x >= d.bounds.x && d.work_area.y >= d.bounds.y);
        DT_CHECK(d.dpi >= 96 && d.scale_factor == d.dpi / 96.0);
        DT_CHECK(d.bounds.x >= virt.x && d.bounds.x + d.bounds.width <= virt.x + virt.width);
        DT_CHECK(d.rotation == 0 || d.rotation == 90 || d.rotation == 180 || d.rotation == 270);
        std::cout << "  " << d.id << " '" << d.label << "' " << d.bounds.width << "x" << d.bounds.height << " @" << d.bounds.x
                  << "," << d.bounds.y << " scale " << d.scale_factor << " " << d.refresh_rate << "Hz" << (d.primary ? " primary" : "") << '\n';
    }
    DT_CHECK(primaries == 1);
    DT_CHECK(all.front().primary);
    const auto primary = primary_display();
    DT_CHECK(primary.has_value() && primary->primary && primary->bounds.x == 0 && primary->bounds.y == 0);
    const auto nearest = display_nearest_point({-100000, -100000});
    DT_CHECK(nearest.has_value());
    const auto at_origin = display_nearest_point({1, 1});
    DT_CHECK(at_origin.has_value() && at_origin->primary);
    const auto by_rect = display_nearest_rect({10, 10, 100, 100});
    DT_CHECK(by_rect.has_value() && by_rect->primary);
    DT_CHECK(!display_for_window(nullptr).has_value());
    const ScreenPoint cursor = cursor_screen_point();
    DT_CHECK(cursor.x >= virt.x - 1 && cursor.x <= virt.x + virt.width + 1);

    // diff_displays
    Display one{.id = "A", .device_name = "A", .bounds = {0, 0, 100, 100}, .work_area = {0, 0, 100, 90}, .primary = true};
    Display two{.id = "B", .device_name = "B", .bounds = {100, 0, 100, 100}, .work_area = {100, 0, 100, 100}};
    Display one_scaled = one;
    one_scaled.dpi = 144;
    one_scaled.scale_factor = 1.5;
    DT_CHECK(diff_displays({one, two}, {one, two}).empty());
    const auto removed = diff_displays({one, two}, {one});
    DT_CHECK(removed.removed.size() == 1 && removed.removed[0].id == "B" && removed.added.empty());
    const auto added = diff_displays({one}, {one, two});
    DT_CHECK(added.added.size() == 1 && added.added[0].id == "B");
    const auto changed = diff_displays({one, two}, {one_scaled, two});
    DT_CHECK(changed.changed.size() == 1 && changed.changed[0].dpi == 144);

    // Watcher: an unchanged configuration produces no event.
    int events = 0;
    DisplayWatcher watcher([&](const ScreenChange&) { ++events; }, nullptr, std::chrono::milliseconds{20});
    DT_CHECK(watcher.hwnd() != nullptr && !watcher.current().empty());
    SendMessageW(watcher.hwnd(), WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
    SendMessageW(watcher.hwnd(), WM_DISPLAYCHANGE, 32, 0);
    pump_for(std::chrono::milliseconds{150});
    DT_CHECK(events == 0);

    boson::core::WindowConfig cfg;
    cfg.title = L"Boson screen test";
    cfg.width = 300;
    cfg.height = 200;
    cfg.quit_on_destroy = false;
    boson::core::Window window(cfg);
    DT_CHECK(window.create(nullptr, SW_HIDE));
    const auto for_window = display_for_window(window.hwnd());
    DT_CHECK(for_window.has_value());
    boson::ipc::ScreenController controller(window);
    boson::ipc::Router<boson::ipc::ScreenController> router(controller);
    const auto rpc_all = router.dispatch_sync(json_for("Screen.get_all_displays"));
    DT_CHECK(is_success(rpc_all) && rpc_all.find(R"("primary":true)") != std::string::npos &&
             rpc_all.find(R"("work_area":{)") != std::string::npos);
    DT_CHECK(is_success(router.dispatch_sync(json_for("Screen.get_display_nearest_point", R"({"x":0,"y":0})"))));
    DT_CHECK(is_success(router.dispatch_sync(json_for("Screen.get_window_display"))));
    DT_CHECK(is_success(router.dispatch_sync(json_for("Screen.get_cursor_screen_point"))));
    window.destroy();
    pump_for(std::chrono::milliseconds{20});
}

// ==== Window state ========================================================================================

void test_window_state_pure() {
    step("window state: fit / load / save");
    using namespace boson::desktop;
    const std::vector<MonitorArea> two{
        {.id = "L", .bounds = {0, 0, 1920, 1080}, .work_area = {0, 0, 1920, 1040}, .dpi = 96},
        {.id = "R", .bounds = {1920, 0, 2560, 1440}, .work_area = {1920, 0, 2560, 1400}, .dpi = 144},
    };
    const WindowState on_right{.x = 2000, .y = 100, .width = 1200, .height = 800, .monitor = "R", .dpi = 144};
    DT_CHECK(fit_window_state(on_right, two) == on_right); // visible: unchanged

    // Right monitor unplugged: moved onto the remaining one, scaled from 144 to 96 DPI.
    const std::vector<MonitorArea> left_only{two[0]};
    const WindowState moved = fit_window_state(on_right, left_only);
    DT_CHECK(moved.monitor == "L" && moved.dpi == 96);
    DT_CHECK(moved.width == 800 && moved.height == 533);
    DT_CHECK(moved.x >= 0 && moved.x + moved.width <= 1920 && moved.y >= 0 && moved.y + moved.height <= 1040);

    // Larger than the work area: shrunk to it.
    const WindowState huge = fit_window_state({.x = 0, .y = 0, .width = 5000, .height = 3000, .monitor = "L", .dpi = 96}, left_only);
    DT_CHECK(huge.width == 1920 && huge.height == 1040 && huge.x == 0 && huge.y == 0);

    // Title bar above the top edge: pulled down.
    const WindowState above = fit_window_state({.x = 100, .y = -500, .width = 800, .height = 600, .monitor = "L", .dpi = 96}, left_only);
    DT_CHECK(above.y == 0 && above.x == 100);
    // Mostly off the right edge with < min_visible px showing: pulled back.
    const WindowState right = fit_window_state({.x = 1900, .y = 100, .width = 800, .height = 600, .monitor = "L", .dpi = 96}, left_only);
    DT_CHECK(right.x == 1920 - 800 && right.y == 100);
    // Partially off-screen but grabbable: left alone (Windows' own invisible borders hang off by ~7px).
    const WindowState edge = fit_window_state({.x = -7, .y = 0, .width = 800, .height = 600, .monitor = "L", .dpi = 96}, left_only);
    DT_CHECK(edge.x == -7);
    // Unknown monitor and no overlap: lands on the first monitor.
    const WindowState lost = fit_window_state({.x = 9000, .y = 9000, .width = 400, .height = 300, .monitor = "X", .dpi = 96}, left_only);
    DT_CHECK(lost.monitor == "L" && lost.x + lost.width <= 1920 && lost.y + lost.height <= 1040);
    DT_CHECK(fit_window_state(on_right, std::vector<MonitorArea>{}) == on_right);

    const auto dir = temp_dir();
    const auto file = dir / L"nested" / L"state.json";
    const WindowState state{.x = 10, .y = 20, .width = 300, .height = 400, .maximized = true, .fullscreen = false, .monitor = "\\\\.\\DISPLAY1", .dpi = 120};
    DT_CHECK(save_window_state(file, state));
    DT_CHECK(!std::filesystem::exists(std::filesystem::path(file).concat(L".tmp")));
    const auto loaded = load_window_state(file);
    DT_CHECK(loaded.has_value() && *loaded == state);
    std::ofstream(file, std::ios::trunc) << "{ not json";
    DT_CHECK(!load_window_state(file).has_value());
    std::ofstream(file, std::ios::trunc) << R"({"x":1,"y":2,"width":0,"height":5})";
    DT_CHECK(!load_window_state(file).has_value());
    std::ofstream(file, std::ios::trunc) << R"({"x":1,"y":2,"width":3,"height":5,"future_field":true})";
    DT_CHECK(load_window_state(file).has_value());
    DT_CHECK(!load_window_state(dir / L"missing.json").has_value());

    const auto default_path = default_window_state_path(L"BosonTestApp", L"main window/1");
    const auto local = known_folder(KnownFolder::LocalAppData);
    DT_CHECK(local.has_value() && default_path.parent_path() == *local / L"BosonTestApp");
    DT_CHECK(default_path.filename() == L"window-state-main_window_1.json");
    DT_CHECK(!std::filesystem::exists(default_path.parent_path())); // computing the path creates nothing

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_window_state_live() {
    step("window state: save / restore a real window");
    using namespace boson::desktop;
    const auto dir = temp_dir();
    const auto file = dir / L"main.json";
    const auto primary = primary_display();
    DT_CHECK(primary.has_value());
    if (!primary) {
        return;
    }
    const auto& wa = primary->work_area;
    const RECT target{wa.x + 40, wa.y + 50, wa.x + 40 + 420, wa.y + 50 + 310};

    boson::core::WindowConfig cfg;
    cfg.title = L"Boson window-state test";
    cfg.width = 320;
    cfg.height = 240;
    cfg.quit_on_destroy = false;
    {
        auto window = std::make_unique<boson::core::Window>(cfg);
        DT_CHECK(window->create(nullptr, SW_HIDE));
        WindowStateKeeper keeper(*window, {.file = file, .debounce = std::chrono::milliseconds{50}});
        DT_CHECK(!keeper.restore(SW_SHOWNOACTIVATE)); // nothing saved yet: just shows it
        DT_CHECK(window->is_visible());
        window->set_bounds(target);
        DT_CHECK(pump_until([&]() { return std::filesystem::exists(file); }, std::chrono::seconds{3})); // debounced save
        const auto saved = load_window_state(file);
        DT_CHECK(saved.has_value() && saved->x == target.left && saved->y == target.top && saved->width == 420 && saved->height == 310);
        DT_CHECK(saved.has_value() && saved->monitor == primary->id && !saved->maximized && !saved->fullscreen);

        const RECT moved{target.left + 30, target.top + 20, target.left + 30 + 420, target.top + 20 + 310};
        window->set_bounds(moved);
        window->destroy(); // final save happens in WM_DESTROY, before the debounce fires
        const auto final_state = load_window_state(file);
        DT_CHECK(final_state.has_value() && final_state->x == moved.left && final_state->y == moved.top);
    }
    {
        auto window = std::make_unique<boson::core::Window>(cfg);
        DT_CHECK(window->create(nullptr, SW_HIDE));
        WindowStateKeeper keeper(*window, {.file = file});
        DT_CHECK(keeper.restore(SW_SHOWNOACTIVATE));
        const RECT bounds = window->bounds();
        DT_CHECK(bounds.left == target.left + 30 && bounds.top == target.top + 20);
        DT_CHECK(bounds.right - bounds.left == 420 && bounds.bottom - bounds.top == 310);
        DT_CHECK(window->is_visible() && !window->is_maximized());
        const auto state = keeper.capture();
        DT_CHECK(state.has_value() && state->x == bounds.left && state->width == 420);
        DT_CHECK(keeper.save_now());
        window->destroy();
    }
    // A state saved on a monitor that no longer exists, far off-screen, comes back visible.
    {
        save_window_state(file, WindowState{.x = -40000, .y = -40000, .width = 500, .height = 400, .monitor = "\\\\.\\DISPLAY99", .dpi = primary->dpi});
        auto window = std::make_unique<boson::core::Window>(cfg);
        DT_CHECK(window->create(nullptr, SW_HIDE));
        WindowStateKeeper keeper(*window, {.file = file});
        DT_CHECK(keeper.restore(SW_HIDE));
        const RECT bounds = window->bounds();
        const RECT work{wa.x, wa.y, wa.x + wa.width, wa.y + wa.height};
        RECT overlap{};
        DT_CHECK(IntersectRect(&overlap, &bounds, &work) && EqualRect(&overlap, &bounds));
        DT_CHECK(!window->is_visible());
        window->destroy();
    }
    pump_for(std::chrono::milliseconds{20});
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_tsgen() {
    step("TypeScript bindings for the desktop controllers and events");
    const std::string dts = boson::ipc::TsGenerator::generate_with_events<
        boson::ipc::DesktopEvents,
        boson::ipc::ClipboardController,
        boson::ipc::ShellController,
        boson::ipc::ScreenController,
        boson::ipc::ShortcutController>();
    for (const std::string_view needle : {
             std::string_view{R"("Clipboard.read_text": () => Promise<ClipboardTextResponse>;)"},
             std::string_view{R"("Shell.open_external": (params: ShellUrlRequest) => Promise<void>;)"},
             std::string_view{R"("Screen.get_all_displays": () => Promise<ScreenDisplaysResponse>;)"},
             std::string_view{R"("Shortcut.register": (params: ShortcutRegisterRequest) => Promise<ShortcutRegisterResponse>;)"},
             std::string_view{R"("clipboard:changed": ClipboardChangedEvent;)"},
             std::string_view{R"("screen:changed": ScreenChangedEvent;)"},
             std::string_view{R"("shortcut:triggered": ShortcutTriggeredEvent;)"},
             std::string_view{"export interface Display {"},
         }) {
        const bool found = dts.find(needle) != std::string::npos;
        if (!found) {
            std::cerr << "  missing in generated d.ts: " << needle << '\n';
        }
        DT_CHECK(found);
    }
}

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    test_cf_html();
    test_base64();
    test_images();
    test_clipboard();
    test_shell();
    test_shortcuts();
    test_deep_links();
    test_screen();
    test_window_state_pure();
    test_window_state_live();
    test_tsgen();

    CoUninitialize();
    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << "All Boson desktop tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
}
