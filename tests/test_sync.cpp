// Synchronous IPC (ipc::SyncBridge host object, BootValues), ElectronIpc::handle_sync and native drag-out
// (transfer::DragOut data objects). The headless part always runs; the WebView part skips when no WebView2
// runtime is installed. Never performs a real mouse drag.
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

namespace st {

struct AddRequest {
    int a = 0;
    int b = 0;
};

struct AddResult {
    int sum = 0;
};

struct ThreadResult {
    std::uint32_t thread_id = 0;
    std::uint64_t sender = 0;
};

struct SleepRequest {
    int ms = 0;
};

class MathController {
public:
    BOSON_CONTROLLER("Math");
    static constexpr std::array<std::string_view, 1> boson_sync_methods() noexcept { return {"add"}; }

    [[nodiscard]] AddResult add(AddRequest req) const { return AddResult{.sum = req.a + req.b}; }
    [[nodiscard]] AddResult mul(AddRequest req) const { return AddResult{.sum = req.a * req.b}; }
};

class ProbeController {
public:
    BOSON_CONTROLLER("Probe");
    static constexpr std::array<std::string_view, 1> boson_sync_methods() noexcept { return {"*"}; }

    [[nodiscard]] ThreadResult thread() const {
        return ThreadResult{.thread_id = static_cast<std::uint32_t>(::GetCurrentThreadId()), .sender = boson::ipc::current_sender_id()};
    }
    void sleep(SleepRequest req) const { std::this_thread::sleep_for(std::chrono::milliseconds{req.ms}); }
    [[nodiscard]] std::string boom() const { throw std::runtime_error("probe exploded"); }
    [[nodiscard]] std::string secret() const { return "s3cret"; }
};

/// No boson_sync_methods(): only what SyncBridge's extra patterns allow.
class PlainController {
public:
    BOSON_CONTROLLER("Plain");
    [[nodiscard]] std::string allowed() const { return "yes"; }
    [[nodiscard]] std::string other() const { return "no"; }
};

struct BootConfig {
    std::string name;
    int answer = 0;
    std::vector<std::string> paths;
};

} // namespace st

template <>
struct glz::meta<st::MathController> {
    using T = st::MathController;
    static constexpr auto value = glz::object("add", &T::add, "mul", &T::mul);
};

template <>
struct glz::meta<st::ProbeController> {
    using T = st::ProbeController;
    static constexpr auto value = glz::object("thread", &T::thread, "sleep", &T::sleep, "boom", &T::boom, "secret", &T::secret);
};

template <>
struct glz::meta<st::PlainController> {
    using T = st::PlainController;
    static constexpr auto value = glz::object("allowed", &T::allowed, "other", &T::other);
};

namespace {

using boson::compat::ElectronIpc;
using boson::compat::IpcEvent;
using boson::ipc::BootValues;
using boson::ipc::SyncBridge;
using boson::transfer::DragOut;
using Microsoft::WRL::ComPtr;
using Json = glz::generic;

int g_failures = 0;
int g_checks = 0;

#ifdef NDEBUG
constexpr std::string_view kBuildType = "Release";
#else
constexpr std::string_view kBuildType = "Debug";
#endif

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_sync.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define ST_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

struct Percentiles {
    double p50 = 0;
    double p95 = 0;
    double p99 = 0;
    double mean = 0;
    double min = 0;
    double max = 0;
};

Percentiles percentiles(std::vector<double> values) {
    Percentiles p;
    if (values.empty()) {
        return p;
    }
    std::ranges::sort(values);
    const auto at = [&](double q) { return values[std::min(values.size() - 1, static_cast<std::size_t>(q * static_cast<double>(values.size())))]; };
    p.p50 = at(0.50);
    p.p95 = at(0.95);
    p.p99 = at(0.99);
    p.mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    p.min = values.front();
    p.max = values.back();
    return p;
}

class Bstr {
public:
    explicit Bstr(const wchar_t* text) : m_value(::SysAllocString(text)) {}
    ~Bstr() { ::SysFreeString(m_value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    [[nodiscard]] BSTR get() const noexcept { return m_value; }

private:
    BSTR m_value;
};

VARIANT bstr_variant(const Bstr& text) {
    VARIANT v;
    ::VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = text.get();
    return v;
}

HRESULT call_invoke(IDispatch* host, std::vector<VARIANT> reversed_args, std::string& out, WORD flags = DISPATCH_METHOD, UINT* arg_error = nullptr) {
    DISPPARAMS params{reversed_args.empty() ? nullptr : reversed_args.data(), nullptr, static_cast<UINT>(reversed_args.size()), 0};
    VARIANT result;
    ::VariantInit(&result);
    UINT error_index = 0;
    const HRESULT hr = host->Invoke(SyncBridge::kInvokeDispId, IID_NULL, LOCALE_USER_DEFAULT, flags, &params, &result, nullptr,
                                    arg_error != nullptr ? arg_error : &error_index);
    out.clear();
    if (SUCCEEDED(hr) && result.vt == VT_BSTR && result.bstrVal != nullptr) {
        out = boson::core::wide_to_utf8(std::wstring_view{result.bstrVal, ::SysStringLen(result.bstrVal)});
    }
    ::VariantClear(&result);
    return hr;
}

// ---- IDispatch host object ------------------------------------------------------------------------------------

void test_host_object() {
    step("SyncBridge host object: IDispatch contract");
    int calls = 0;
    std::string last_method;
    std::string last_params;
    ComPtr<IDispatch> host = SyncBridge::make_host_object([&](std::string_view method, std::string_view params) {
        ++calls;
        last_method = method;
        last_params = params;
        return std::string{"{\"ok\":\"h\xC3\xA9\xE2\x82\xAC\"}"}; // "hé€"
    });
    ST_CHECK(host != nullptr);
    ComPtr<IUnknown> unknown;
    ST_CHECK(SUCCEEDED(host.As(&unknown)));

    UINT type_infos = 7;
    ST_CHECK(host->GetTypeInfoCount(&type_infos) == S_OK && type_infos == 0);

    std::wstring invoke_name = L"invoke";
    LPOLESTR names[1] = {invoke_name.data()};
    DISPID id = 0;
    ST_CHECK(host->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id) == S_OK && id == SyncBridge::kInvokeDispId);
    std::wstring other_name = L"then";
    names[0] = other_name.data();
    ST_CHECK(host->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id) == DISP_E_UNKNOWNNAME && id == DISPID_UNKNOWN);

    const Bstr method{L"Math.add"};
    const Bstr params{L"{\"a\":1,\"t\":\"\u00e9\"}"};
    std::string out;
    ST_CHECK(call_invoke(host.Get(), {bstr_variant(params), bstr_variant(method)}, out) == S_OK);
    ST_CHECK(out == "{\"ok\":\"h\xC3\xA9\xE2\x82\xAC\"}");
    ST_CHECK(last_method == "Math.add" && last_params == "{\"a\":1,\"t\":\"\xC3\xA9\"}");

    ST_CHECK(call_invoke(host.Get(), {bstr_variant(method)}, out) == S_OK && last_params.empty());
    VARIANT missing;
    ::VariantInit(&missing);
    missing.vt = VT_ERROR;
    missing.scode = DISP_E_PARAMNOTFOUND;
    ST_CHECK(call_invoke(host.Get(), {missing, bstr_variant(method)}, out) == S_OK && last_params.empty());

    VARIANT number;
    ::VariantInit(&number);
    number.vt = VT_I4;
    number.lVal = 5;
    UINT bad_index = 99;
    ST_CHECK(call_invoke(host.Get(), {number}, out, DISPATCH_METHOD, &bad_index) == DISP_E_TYPEMISMATCH && bad_index == 0);
    ST_CHECK(call_invoke(host.Get(), {number, bstr_variant(method)}, out, DISPATCH_METHOD, &bad_index) == DISP_E_TYPEMISMATCH && bad_index == 0);
    ST_CHECK(call_invoke(host.Get(), {}, out) == DISP_E_BADPARAMCOUNT);
    ST_CHECK(call_invoke(host.Get(), {bstr_variant(method), bstr_variant(method), bstr_variant(method)}, out) == DISP_E_BADPARAMCOUNT);
    ST_CHECK(call_invoke(host.Get(), {bstr_variant(method)}, out, DISPATCH_PROPERTYGET) == DISP_E_MEMBERNOTFOUND);

    VARIANT arg = bstr_variant(method);
    DISPID named = 0;
    DISPPARAMS with_names{&arg, &named, 1, 1};
    VARIANT result;
    ::VariantInit(&result);
    ST_CHECK(host->Invoke(SyncBridge::kInvokeDispId, IID_NULL, 0, DISPATCH_METHOD, &with_names, &result, nullptr, nullptr) == DISP_E_NONAMEDARGS);
    ST_CHECK(host->Invoke(42, IID_NULL, 0, DISPATCH_METHOD, &with_names, &result, nullptr, nullptr) == DISP_E_MEMBERNOTFOUND);
    ST_CHECK(calls == 3);

    // A handler that throws never lets the exception cross the COM boundary.
    ComPtr<IDispatch> throwing = SyncBridge::make_host_object([](std::string_view, std::string_view) -> std::string { throw std::runtime_error("x"); });
    ST_CHECK(call_invoke(throwing.Get(), {bstr_variant(method)}, out) == E_FAIL);
}

// ---- Sync dispatch pipeline -----------------------------------------------------------------------------------

void test_sync_core() {
    step("SyncBridge: allow-list, origin, capabilities, errors, sender, ElectronIpc.sendSync");
    st::MathController math;
    st::ProbeController probe;
    st::PlainController plain;
    ElectronIpc ipc;
    boson::ipc::Router<st::MathController, st::ProbeController, st::PlainController, ElectronIpc> router(math, probe, plain, ipc);
    SyncBridge sync(router, {"Plain.allowed", "not a pattern!"});

    ST_CHECK(sync.is_sync_method("Math.add"));
    ST_CHECK(!sync.is_sync_method("Math.mul"));
    ST_CHECK(sync.is_sync_method("Probe.thread") && sync.is_sync_method("Probe.secret"));
    ST_CHECK(sync.is_sync_method("ElectronIpc.sendSync") && !sync.is_sync_method("ElectronIpc.invoke"));
    ST_CHECK(sync.is_sync_method("Plain.allowed") && !sync.is_sync_method("Plain.other"));
    ST_CHECK(sync.is_sync_method("SyncHost.zoom_factor"));
    ST_CHECK(SyncBridge::is_valid_method_name("A.b") && !SyncBridge::is_valid_method_name("Ab") &&
             !SyncBridge::is_valid_method_name("A.b\"") && !SyncBridge::is_valid_method_name(".b") &&
             !SyncBridge::is_valid_method_name(std::string(300, 'a') + ".b"));

    const std::vector<std::string> origins{"boson://app"};
    constexpr std::string_view kSource = "boson://app/index.html";
    std::string r = sync.invoke_checked(origins, nullptr, 5, kSource, "Math.add", R"({"a":2,"b":40})");
    ST_CHECK(contains(r, R"("status":"success")") && contains(r, R"("sum":42)"));
    r = sync.invoke_checked(origins, nullptr, 5, kSource, "Math.add", "");
    ST_CHECK(contains(r, R"("sum":0)"));
    r = sync.invoke_checked(origins, nullptr, 5, kSource, "Math.mul", R"({"a":2,"b":3})");
    ST_CHECK(contains(r, R"("code":-32005)") && contains(r, "not enabled for synchronous calls"));
    r = sync.invoke_checked(origins, nullptr, 5, "https://evil.example/x", "Math.add", "{}");
    ST_CHECK(contains(r, R"("code":-32003)") && contains(r, "untrusted origin"));
    r = sync.invoke_checked(origins, nullptr, 5, kSource, "Math", "{}");
    ST_CHECK(contains(r, R"("code":-32601)"));
    r = sync.invoke_checked(origins, nullptr, 5, kSource, "Ma th.add", "{}");
    ST_CHECK(contains(r, R"("code":-32601)"));
    r = sync.invoke_checked(origins, nullptr, 5, kSource, "Math.add", "{bad");
    ST_CHECK(contains(r, R"("status":"error")") && contains(r, "-32700"));
    r = sync.invoke_checked(origins, nullptr, 5, kSource, "Plain.allowed", "{}");
    ST_CHECK(contains(r, R"("data":"yes")"));
    r = sync.invoke_checked(origins, nullptr, 5, kSource, "Plain.other", "{}");
    ST_CHECK(contains(r, R"("code":-32005)"));
    r = sync.invoke_checked(origins, nullptr, 5, kSource, "Probe.boom", "{}");
    ST_CHECK(contains(r, R"("status":"error")") && contains(r, "probe exploded"));

    r = sync.invoke_checked(origins, nullptr, 77, kSource, "Probe.thread", "{}");
    Json parsed{};
    ST_CHECK(!glz::read_json(parsed, r));
    ST_CHECK(parsed["data"]["sender"].get_number() == 77.0);
    ST_CHECK(static_cast<DWORD>(parsed["data"]["thread_id"].get_number()) == ::GetCurrentThreadId());
    ST_CHECK(boson::ipc::current_sender_id() == 0); // restored after the call

    // Capability policy on the sync path: deny wins, not-granted is rejected too.
    const auto policy = boson::security::CapabilityPolicy::shared_from_json(
        R"({"default_deny":true,"capabilities":[{"identifier":"main","origins":["boson://app"],"allow":["Math.*","Probe.*"],"deny":["Probe.secret"]}]})");
    ST_CHECK(policy.has_value());
    r = sync.invoke_checked(origins, policy->get(), 5, kSource, "Math.add", R"({"a":1,"b":1})");
    ST_CHECK(contains(r, R"("sum":2)"));
    r = sync.invoke_checked(origins, policy->get(), 5, kSource, "Probe.secret", "{}");
    ST_CHECK(contains(r, R"("code":-32003)") && contains(r, "denied by capability rule") && !contains(r, "s3cret"));
    r = sync.invoke_checked(origins, policy->get(), 5, kSource, "ElectronIpc.sendSync", R"({"channel":"x","args":[]})");
    ST_CHECK(contains(r, R"("code":-32003)") && contains(r, "not granted"));

    // ElectronIpc.sendSync -> handle_sync handlers (separate from handle()).
    ST_CHECK(ipc.handle_sync("get-config", [](const IpcEvent& e, Json args) {
        Json out;
        out["sender"] = static_cast<double>(e.sender_id);
        out["first"] = args.get_array().empty() ? Json{} : args[0];
        return out;
    }));
    ST_CHECK(!ipc.handle_sync("get-config", [](const IpcEvent&, Json) { return Json{}; }));
    ST_CHECK(ipc.has_sync_handler("get-config") && !ipc.has_handler("get-config"));
    r = sync.invoke_checked(origins, nullptr, 9, kSource, "ElectronIpc.sendSync", R"({"channel":"get-config","args":["a"]})");
    ST_CHECK(contains(r, R"("sender":9)") && contains(r, R"("first":"a")"));
    r = sync.invoke_checked(origins, nullptr, 9, kSource, "ElectronIpc.sendSync", R"({"channel":"nope","args":[]})");
    ST_CHECK(contains(r, R"("code":-32601)") && contains(r, "handle_sync"));
    r = router.dispatch_sync(R"({"id":1,"method":"ElectronIpc.invoke","params":{"channel":"get-config","args":[]}})");
    ST_CHECK(contains(r, R"("code":-32601)")); // invoke() does not see sync handlers
    ST_CHECK(ipc.handle_sync_raw("raw", [](const IpcEvent&, std::string_view args) { return std::string{args}; }));
    r = sync.invoke_checked(origins, nullptr, 9, kSource, "ElectronIpc.sendSync", R"({"channel":"raw","args":[1,2]})");
    ST_CHECK(contains(r, R"("data":[1,2])"));
    ST_CHECK(ipc.remove_sync_handler("raw") && !ipc.remove_sync_handler("raw"));

    // SyncHost needs an attached WebView; through the async path it refuses to run at all.
    r = sync.invoke_checked(origins, nullptr, 5, kSource, "SyncHost.zoom_factor", "{}");
    ST_CHECK(contains(r, R"("status":"error")") && contains(r, "not attached"));
    boson::ipc::SyncHostController host_controller(&sync);
    boson::ipc::Router<boson::ipc::SyncHostController> host_router(host_controller);
    auto off_thread = std::async(std::launch::async, [&host_router]() {
        return host_router.dispatch_sync(R"({"id":3,"method":"SyncHost.zoom_factor","params":{}})");
    });
    r = off_thread.get();
    ST_CHECK(contains(r, R"("status":"error")") && contains(r, "only available through boson.invokeSync"));

    // Budget accounting.
    const auto before = sync.stats();
    sync.set_budget(std::chrono::milliseconds{1});
    r = sync.invoke_checked(origins, nullptr, 5, kSource, "Probe.sleep", R"({"ms":5})");
    ST_CHECK(contains(r, R"("status":"success")"));
    ST_CHECK(sync.stats().over_budget == before.over_budget + 1);
    ST_CHECK(sync.stats().max_ns >= 5'000'000);
    ST_CHECK(sync.stats().rejected >= 6);
    sync.set_budget(std::chrono::milliseconds{2});

    // Hot-path cost of the C++ side (checks + envelope + Router + glaze), and of the IDispatch layer on top.
    constexpr int kIterations = 200'000;
    std::size_t sink = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        sink += sync.invoke_checked(origins, policy->get(), 5, kSource, "Math.add", R"({"a":1,"b":2})").size();
    }
    const double core_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kIterations;
    ComPtr<IDispatch> host = SyncBridge::make_host_object([&](std::string_view method, std::string_view params) {
        return sync.invoke_checked(origins, policy->get(), 5, kSource, method, params);
    });
    const Bstr method{L"Math.add"};
    const Bstr params{L"{\"a\":1,\"b\":2}"};
    std::string out;
    start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        call_invoke(host.Get(), {bstr_variant(params), bstr_variant(method)}, out);
        sink += out.size();
    }
    const double dispatch_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kIterations;
    ST_CHECK(sink > 0 && contains(out, R"("sum":3)"));
    std::cout << std::fixed << std::setprecision(0) << "  [perf] sync C++ path (origin+allow-list+capability+Router+glaze): " << core_ns
              << " ns/call; via IDispatch::Invoke incl. UTF-16<->UTF-8 + BSTR: " << dispatch_ns << " ns/call (" << kIterations
              << " calls, " << kBuildType << " build)\n";
}

// ---- Boot values ----------------------------------------------------------------------------------------------

void test_boot_values() {
    step("BootValues: JSON, sendSync replies, process info, script");
    BootValues boot;
    ST_CHECK(boot.set("config", st::BootConfig{.name = "Scribe", .answer = 42, .paths = {"C:\\Users\\x", "D:\\y"}}));
    ST_CHECK(boot.set_json("flags", R"({"beta":true})"));
    ST_CHECK(!boot.set_json("bad", "{nope"));
    ST_CHECK(!boot.set_json("", "1"));
    ST_CHECK(!boot.set_json("sendSync", "1"));
    ST_CHECK(boot.set_send_sync("get-path", std::string{"C:\\data"}));
    ST_CHECK(boot.set_send_sync_json("get-flags", R"([1,2])"));
    ST_CHECK(!boot.set_send_sync_json("x", "]"));
    ST_CHECK(boot.set_process_info("1.2.3"));
    ST_CHECK(boot.contains("config") && boot.contains("sendSync") && boot.contains("process") && !boot.contains("bad"));
    ST_CHECK(boot.size() == 4);

    std::string json = boot.to_json();
    Json parsed{};
    ST_CHECK(!glz::read_json(parsed, json));
    ST_CHECK(parsed["config"]["name"].get_string() == "Scribe" && parsed["config"]["answer"].get_number() == 42.0);
    ST_CHECK(parsed["config"]["paths"][0].get_string() == "C:\\Users\\x");
    ST_CHECK(parsed["sendSync"]["get-path"].get_string() == "C:\\data");
    ST_CHECK(parsed["process"]["platform"].get_string() == "win32" && parsed["process"]["versions"]["app"].get_string() == "1.2.3");
    ST_CHECK(!parsed["process"]["arch"].get_string().empty());
    ST_CHECK(json.find("\"config\"") < json.find("\"flags\"") && json.find("\"process\"") < json.find("\"sendSync\""));

    ST_CHECK(boot.set_json("flags", "false"));
    json = boot.to_json();
    ST_CHECK(contains(json, R"("flags":false)") && json.find("\"config\"") < json.find("\"flags\""));
    ST_CHECK(boot.remove("flags") && !boot.remove("flags") && !contains(boot.to_json(), "\"flags\":"));

    std::string script = boot.script();
    ST_CHECK(contains(script, "__BOSON_BOOT__") && contains(script, R"(const a=["boson://app"])"));
    ST_CHECK(contains(script, "Object.freeze") && contains(script, R"("get-path":"C:\\data")"));
    boot.set_allowed_origins({"https://App.Example/", "*"});
    script = boot.script();
    ST_CHECK(contains(script, R"(const a=["https://app.example","*"])"));
    ST_CHECK(boot.wide_script().size() == script.size()); // ASCII-only script here
    ST_CHECK(boot.remove("sendSync") && !boot.contains("sendSync"));
    boot.clear();
    ST_CHECK(boot.size() == 0 && boot.to_json() == "{}");
}

// ---- Drag-out data object ---------------------------------------------------------------------------------------

/// 32-bpp BI_RGB bottom-up BMP with a gradient.
void write_bmp(const std::filesystem::path& path, int width, int height) {
    const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(width * height * 4);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + pixel_bytes;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(BITMAPINFOHEADER);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::vector<std::uint8_t> pixels(pixel_bytes);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>((y * width + x) * 4);
            pixels[i] = static_cast<std::uint8_t>(x * 255 / std::max(1, width - 1));
            pixels[i + 1] = static_cast<std::uint8_t>(y * 255 / std::max(1, height - 1));
            pixels[i + 2] = 0x80;
            pixels[i + 3] = 0xFF;
        }
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}

bool same_paths(const std::vector<std::wstring>& actual, const std::vector<std::filesystem::path>& expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (::CompareStringOrdinal(actual[i].c_str(), -1, expected[i].c_str(), -1, TRUE) != CSTR_EQUAL) {
            return false;
        }
    }
    return true;
}

void test_drag_out() {
    step("DragOut: shell data object formats, drag image, validation (no real drag)");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / (L"boson-dragout-" + std::to_wstring(::GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(dir / L"sub" / L"folder", ec);
    const auto a = dir / L"a.txt";
    const auto b = dir / L"b \u00e9t\u00e9.txt"; // non-ASCII name
    const auto c = dir / L"sub" / L"c.txt";
    const auto folder = dir / L"sub" / L"folder";
    for (const auto& file : {a, b, c}) {
        std::ofstream(file) << "boson";
    }

    ComPtr<IDataObject> data;
    const std::vector<std::filesystem::path> same_folder{a, b};
    ST_CHECK(SUCCEEDED(DragOut::create_data_object(same_folder, data)) && data);
    ST_CHECK(same_paths(DragOut::hdrop_paths(data.Get()), same_folder));
    ST_CHECK(DragOut::has_format(data.Get(), static_cast<CLIPFORMAT>(CF_HDROP)));
    ST_CHECK(DragOut::has_format(data.Get(), CFSTR_SHELLIDLIST));
    ST_CHECK(DragOut::dword_format(data.Get(), CFSTR_PREFERREDDROPEFFECT) == std::optional<DWORD>{DROPEFFECT_COPY});
    std::cout << "  data object formats: CF_HDROP, Shell IDList Array, Preferred DropEffect"
              << (DragOut::has_format(data.Get(), CFSTR_FILENAMEW) ? ", FileNameW" : "") << '\n';

    const std::vector<std::filesystem::path> mixed{a, c, folder};
    ComPtr<IDataObject> mixed_data;
    ST_CHECK(SUCCEEDED(DragOut::create_data_object(mixed, mixed_data)) && mixed_data);
    ST_CHECK(same_paths(DragOut::hdrop_paths(mixed_data.Get()), mixed));
    ST_CHECK(DragOut::has_format(mixed_data.Get(), CFSTR_SHELLIDLIST));

    ComPtr<IDataObject> none;
    ST_CHECK(DragOut::create_data_object({}, none) == E_INVALIDARG && !none);
    const std::vector<std::filesystem::path> missing{dir / L"missing.txt"};
    ST_CHECK(FAILED(DragOut::create_data_object(missing, none)) && !none);

    // Drag image: decoded with WIC, attached with IDragSourceHelper (stored in the data object).
    const auto icon = dir / L"icon.bmp";
    const auto big = dir / L"big.bmp";
    write_bmp(icon, 48, 32);
    write_bmp(big, 600, 300);
    ST_CHECK(!DragOut::has_format(data.Get(), L"DragImageBits"));
    ST_CHECK(SUCCEEDED(DragOut::set_drag_image(data.Get(), icon)));
    ST_CHECK(DragOut::has_format(data.Get(), L"DragImageBits"));
    ST_CHECK(SUCCEEDED(DragOut::set_drag_image(mixed_data.Get(), big, POINT{10, 10})));
    ST_CHECK(DragOut::has_format(mixed_data.Get(), L"DragImageBits"));
    ST_CHECK(FAILED(DragOut::set_drag_image(data.Get(), a))); // not an image
    ST_CHECK(DragOut::set_drag_image(nullptr, icon) == E_INVALIDARG);

    ST_CHECK(DragOut::parse_effects(std::vector<std::string>{"copy", "move"}) == (DROPEFFECT_COPY | DROPEFFECT_MOVE));
    ST_CHECK(DragOut::parse_effects(std::vector<std::string>{"copy", "bogus"}) == 0);
    ST_CHECK(boson::transfer::to_string(boson::transfer::DragEffect::Link) == "link");

    // run() validates before it would enter the modal loop.
    auto result = DragOut::run(nullptr, {.files = {}, .icon = {}});
    ST_CHECK(!result.started && contains(result.error, "no files"));
    result = DragOut::run(nullptr, {.files = {L"relative.txt"}});
    ST_CHECK(!result.started && contains(result.error, "not an existing absolute path"));
    result = DragOut::run(nullptr, {.files = {a}, .icon = {}, .allowed_effects = 0});
    ST_CHECK(!result.started && contains(result.error, "no drop effect"));
    const bool button_down = DragOut::is_mouse_button_down();
    if (!button_down) {
        result = DragOut::run(nullptr, {.files = {a}});
        ST_CHECK(!result.started && contains(result.error, "no mouse button is down"));
    } else {
        std::cout << "  SKIP (button-up check): a mouse button is held down\n";
    }

    // start(): queued on the UI thread through UiMarshal, outcome delivered to the callback there.
    boson::exec::UiMarshal marshal([]() {});
    DragOut drag(marshal, nullptr);
    std::optional<boson::transfer::DragOutResult> async_result;
    DWORD callback_thread = 0;
    ST_CHECK(drag.start({.files = {dir / L"nope.txt"}}, [&](const boson::transfer::DragOutResult& r) {
        async_result = r;
        callback_thread = ::GetCurrentThreadId();
    }));
    ST_CHECK(!async_result.has_value()); // nothing runs inside start()
    marshal.drain_on_ui_thread();
    ST_CHECK(async_result.has_value() && !async_result->started && callback_thread == ::GetCurrentThreadId());
    ST_CHECK(!drag.is_dragging());

    // Drag controller: validation on the calling (pool) thread, FsScope containment.
    boson::ipc::DragController controller(drag, nullptr);
    const auto throws = [](auto&& fn) {
        try {
            fn();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    ST_CHECK(throws([&] { static_cast<void>(controller.start({.files = {}})); }));
    ST_CHECK(throws([&] { static_cast<void>(controller.start({.files = {"relative.txt"}})); }));
    ST_CHECK(throws([&] { static_cast<void>(controller.start({.files = {boson::core::wide_to_utf8((dir / L"missing").wstring())}})); }));
    ST_CHECK(throws([&] { static_cast<void>(controller.start({.files = {boson::core::wide_to_utf8(a.wstring())}, .icon = {}, .effects = {"bogus"}})); }));
    const auto first = controller.start({.files = {boson::core::wide_to_utf8(a.wstring())}});
    const auto second = controller.start({.files = {boson::core::wide_to_utf8(b.wstring())}, .icon = {}, .effects = {"copy", "move"}});
    ST_CHECK(first.drag_id >= 1 && second.drag_id == first.drag_id + 1);
    marshal.drain_on_ui_thread(); // both rejected (no button) or, with a button held, would start a drag

    auto scope = std::make_shared<boson::security::FsScope>();
    ST_CHECK(scope->allow((dir / L"sub").wstring(), true).has_value());
    boson::ipc::DragController scoped(drag, nullptr, scope);
    ST_CHECK(throws([&] { static_cast<void>(scoped.start({.files = {boson::core::wide_to_utf8(a.wstring())}})); }));
    ST_CHECK(!throws([&] { static_cast<void>(scoped.start({.files = {boson::core::wide_to_utf8(c.wstring())}})); }));
    marshal.drain_on_ui_thread();

    data.Reset();
    mixed_data.Reset();
    std::filesystem::remove_all(dir, ec);
}

// ---- WebView part -----------------------------------------------------------------------------------------------

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    return true;
}

std::optional<std::string> eval(boson::core::WebView& webview, std::string_view js) {
    bool done = false;
    std::optional<std::string> result;
    if (!webview.execute_script(js, [&](HRESULT hr, std::string value) {
            if (SUCCEEDED(hr)) {
                result = std::move(value);
            }
            done = true;
        })) {
        return std::nullopt;
    }
    pump_until([&]() { return done; }, std::chrono::seconds{30});
    return result;
}

Json eval_json(boson::core::WebView& webview, std::string_view js) {
    Json out{};
    if (const auto text = eval(webview, js)) {
        if (glz::read_json(out, *text)) {
            std::cerr << "  (eval_json: unparsable result " << text->substr(0, 200) << ")\n";
        }
    }
    return out;
}

/// The sync host object's response envelope, parsed (invoke returns a JSON string; ExecuteScript wraps it again).
Json sync_call(boson::core::WebView& webview, std::string_view method, std::string_view params_js_expr) {
    std::string js = "(()=>{try{return JSON.parse(chrome.webview.hostObjects.sync.boson.invoke('";
    js.append(method);
    js.append("', JSON.stringify(");
    js.append(params_js_expr);
    js.append(")));}catch(e){return {thrown:String(e&&e.message||e)};}})()");
    return eval_json(webview, js);
}

const std::string kPage =
    "<!doctype html><html><head><title>sync</title><script>"
    "window.__bootAtParse = (typeof window.__BOSON_BOOT__ === 'object') ? window.__BOSON_BOOT__.config.name : 'missing';"
    "window.__pending = {}; window.__id = 0;"
    "window.__rpc = (method, params) => new Promise((resolve) => { const id = ++window.__id; window.__pending[id] = resolve;"
    " chrome.webview.postMessage({ id, method, params }); });"
    "chrome.webview.addEventListener('message', (e) => { const d = e.data; if (d && d.id && window.__pending[d.id]) {"
    " window.__pending[d.id](d); delete window.__pending[d.id]; } });"
    "window.__stats = (ts) => { const s = [...ts].sort((x, y) => x - y); const at = (q) => s[Math.min(s.length - 1, Math.floor(q * s.length))];"
    " return { n: s.length, p50: at(0.5), p95: at(0.95), p99: at(0.99), min: s[0], max: s[s.length - 1],"
    " mean: s.reduce((a, b) => a + b, 0) / s.length }; };"
    "</script></head><body>sync</body></html>";

void print_stats(std::string_view label, const Json& stats) {
    if (!stats.is_object()) {
        std::cout << "  [perf] " << label << ": n/a\n";
        return;
    }
    const auto num = [&](const char* key) {
        const auto& obj = stats.get_object();
        const auto it = obj.find(key);
        return it != obj.end() && it->second.is_number() ? it->second.get_number() : -1.0;
    };
    std::cout << std::fixed << std::setprecision(3) << "  [perf] " << label << ": n=" << num("n") << " p50=" << num("p50")
              << " ms p95=" << num("p95") << " ms p99=" << num("p99") << " ms mean=" << num("mean") << " ms min=" << num("min")
              << " ms max=" << num("max") << " ms\n";
}

void test_webview() {
    step("SyncBridge + BootValues in a real WebView2");
    {
        LPWSTR version = nullptr;
        if (FAILED(::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
            std::cout << "SKIP (WebView part): no WebView2 runtime installed\n";
            return;
        }
        std::wcout << L"  WebView2 runtime " << version << L'\n';
        ::CoTaskMemFree(version);
    }
    const std::filesystem::path user_data = std::filesystem::temp_directory_path() / (L"boson-sync-" + std::to_wstring(::GetCurrentProcessId()));
    {
        boson::core::WindowConfig window_cfg;
        window_cfg.title = L"Boson sync IPC test";
        window_cfg.width = 640;
        window_cfg.height = 480;
        window_cfg.quit_on_destroy = false;
        auto window = std::make_unique<boson::core::Window>(window_cfg);
        ST_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

        boson::exec::Scheduler scheduler(2);
        boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
        window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
        boson::exec::TaskRunner runner(scheduler, marshal);

        boson::core::EnvironmentConfig env_cfg;
        env_cfg.user_data_folder = user_data.wstring();
        const auto env = boson::core::Environment::create(env_cfg);
        auto host = std::make_unique<boson::core::WebViewHost>(*window, env);
        host->set_default_asset_lookup([](std::string_view path) -> std::optional<std::span<const std::byte>> {
            if (path == "/index.html") {
                return std::as_bytes(std::span{kPage.data(), kPage.size()});
            }
            return std::nullopt;
        });

        BootValues boot;
        ST_CHECK(boot.set("config", st::BootConfig{.name = "Scribe", .answer = 42, .paths = {"C:\\Users\\x"}}));
        ST_CHECK(boot.set_send_sync("ping", std::string{"pong"}));
        ST_CHECK(boot.set_process_info("9.9.9"));
        boson::core::WebViewConfig cfg_a;
        cfg_a.scripts_on_document_created.push_back(boot.wide_script());

        bool a_nav = false;
        bool b_nav = false;
        boson::core::WebView& a = host->create(std::move(cfg_a));
        boson::core::WebView& b = host->create(boson::core::WebViewConfig{});
        a.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { a_nav = e.is_success; };
        b.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { b_nav = e.is_success; };
        host->set_layout([&a, &b](boson::core::WebViewHost&, const boson::core::LayoutContext& ctx) {
            a.set_bounds(RECT{0, 0, ctx.width() / 2, ctx.height()});
            b.set_bounds(RECT{ctx.width() / 2, 0, ctx.width(), ctx.height()});
        });
        ST_CHECK(pump_until([&]() { return a_nav && b_nav; }));

        st::MathController math;
        st::ProbeController probe;
        st::PlainController plain;
        ElectronIpc ipc;
        boson::ipc::Router<st::MathController, st::ProbeController, st::PlainController, ElectronIpc> router(math, probe, plain, ipc);
        const auto policy = boson::security::CapabilityPolicy::shared_from_json(
            R"({"default_deny":true,"capabilities":[{"identifier":"main","origins":["boson://app"],"allow":["Math.*","Probe.*","ElectronIpc.*","SyncHost.*"],"deny":["Probe.secret"]}]})");
        ST_CHECK(policy.has_value());
        auto bridge = std::make_unique<boson::ipc::Bridge>(runner, marshal, router);
        bridge->attach(a, {.allowed_origins = {}, .capabilities = *policy});
        bridge->attach(b);
        auto sync = std::make_unique<SyncBridge>(router);
        ST_CHECK(sync->attach(a, {.allowed_origins = {}, .capabilities = *policy}));
        ST_CHECK(sync->attach(b));
        ST_CHECK(sync->is_attached(a.id()) && sync->is_attached(b.id()));
        ipc.set_bridge(bridge.get());
        ST_CHECK(ipc.handle_sync("whoami", [](const IpcEvent& e, Json) { return Json(static_cast<double>(e.sender_id)); }));

        // Boot values: defined before the page's first script, deep-frozen, origin-gated.
        ST_CHECK(eval(a, "window.__bootAtParse") == std::optional<std::string>{"\"Scribe\""});
        ST_CHECK(eval(a, "Object.isFrozen(__BOSON_BOOT__) && Object.isFrozen(__BOSON_BOOT__.config) && Object.isFrozen(__BOSON_BOOT__.config.paths)") ==
                 std::optional<std::string>{"true"});
        ST_CHECK(eval(a, "(()=>{try{__BOSON_BOOT__.config.name='x';}catch(e){} try{window.__BOSON_BOOT__=1;}catch(e){} return __BOSON_BOOT__.config.name;})()") ==
                 std::optional<std::string>{"\"Scribe\""});
        ST_CHECK(eval(a, "__BOSON_BOOT__.sendSync.ping + '|' + __BOSON_BOOT__.process.platform + '|' + __BOSON_BOOT__.process.versions.app") ==
                 std::optional<std::string>{"\"pong|win32|9.9.9\""});
        const Json boot_read = eval_json(a, "(()=>{const t0=performance.now();let s=0;for(let i=0;i<100000;i++){s+=__BOSON_BOOT__.config.answer;}"
                                            "return {ms:performance.now()-t0,s};})()");
        if (boot_read.is_object()) {
            std::cout << std::fixed << std::setprecision(1) << "  [perf] boot value read: "
                      << boot_read.get_object().at("ms").get_number() * 1e6 / 100000.0 << " ns/read (100000 reads)\n";
        }
        ST_CHECK(eval(b, "typeof window.__BOSON_BOOT__") == std::optional<std::string>{"\"undefined\""});
        ST_CHECK(boot.install(b, true));
        ST_CHECK(pump_until([&]() { return eval(b, "typeof window.__BOSON_BOOT__") == std::optional<std::string>{"\"object\""}; }, std::chrono::seconds{5}));

        // Cold call: first proxy creation + first invoke in this document.
        const Json cold = eval_json(a, "(()=>{const t0=performance.now();const h=chrome.webview.hostObjects.sync.boson;const t1=performance.now();"
                                       "const r=JSON.parse(h.invoke('Math.add', JSON.stringify({a:2,b:3})));const t2=performance.now();"
                                       "return {proxy:t1-t0, call:t2-t1, sum:r.data.sum};})()");
        ST_CHECK(cold.is_object() && cold.get_object().at("sum").get_number() == 5.0);
        if (cold.is_object()) {
            std::cout << std::fixed << std::setprecision(3) << "  [perf] sync cold: proxy " << cold.get_object().at("proxy").get_number()
                      << " ms, first invoke " << cold.get_object().at("call").get_number() << " ms\n";
        }
        const Json hot = eval_json(a, "(()=>{const h=chrome.webview.hostObjects.sync.boson;const ts=[];let ok=0;const p=JSON.stringify({a:1,b:2});"
                                      "for(let i=0;i<2000;i++){const t0=performance.now();const r=h.invoke('Math.add',p);ts.push(performance.now()-t0);"
                                      "if(r.indexOf('\"sum\":3')>=0)ok++;}const s=__stats(ts);s.ok=ok;return s;})()");
        ST_CHECK(hot.is_object() && hot.get_object().at("ok").get_number() == 2000.0);
        print_stats("sync hot invoke (Math.add, JS-measured round trip)", hot);
        const Json zoom_hot = eval_json(a, "(()=>{const h=chrome.webview.hostObjects.sync.boson;const ts=[];"
                                           "for(let i=0;i<500;i++){const t0=performance.now();h.invoke('SyncHost.zoom_factor','{}');ts.push(performance.now()-t0);}"
                                           "return __stats(ts);})()");
        print_stats("sync hot invoke (SyncHost.zoom_factor)", zoom_hot);

        eval(a, "window.__asyncStats=null;(async()=>{const ts=[];for(let i=0;i<500;i++){const t0=performance.now();"
                "await window.__rpc('Math.add',{a:i,b:1});ts.push(performance.now()-t0);}window.__asyncStats=__stats(ts);})();1");
        ST_CHECK(pump_until([&]() { return eval(a, "window.__asyncStats !== null").value_or("") == "true"; }, std::chrono::seconds{60}));
        print_stats("async boson.call round trip (Math.add, postMessage + pool thread)", eval_json(a, "window.__asyncStats"));

        // Threads: sync handlers run on the UI thread (this thread); async handlers on a pool thread.
        const Json thread = sync_call(a, "Probe.thread", "{}");
        ST_CHECK(thread.is_object() && thread.get_object().contains("data"));
        if (thread.is_object() && thread.get_object().contains("data")) {
            ST_CHECK(static_cast<DWORD>(thread["data"]["thread_id"].get_number()) == ::GetCurrentThreadId());
            ST_CHECK(static_cast<std::uint64_t>(thread["data"]["sender"].get_number()) == a.id());
        }
        eval(a, "window.__asyncThread=null;window.__rpc('Probe.thread',{}).then(r=>{window.__asyncThread=r.data.thread_id;});1");
        ST_CHECK(pump_until([&]() { return eval(a, "window.__asyncThread !== null").value_or("") == "true"; }, std::chrono::seconds{10}));
        const auto async_thread = eval(a, "window.__asyncThread").value_or("0");
        ST_CHECK(async_thread != "0" && async_thread != std::to_string(::GetCurrentThreadId()));

        // The page's JS thread is blocked for the whole handler: a 0 ms timer cannot fire during the call.
        const Json blocked = eval_json(a, "(()=>{let fired=false;setTimeout(()=>{fired=true;},0);const t0=performance.now();"
                                          "chrome.webview.hostObjects.sync.boson.invoke('Probe.sleep', JSON.stringify({ms:30}));"
                                          "return {ms:performance.now()-t0, fired};})()");
        ST_CHECK(blocked.is_object() && blocked.get_object().at("ms").get_number() >= 29.0 && !blocked.get_object().at("fired").get<bool>());
        ST_CHECK(sync->stats().over_budget >= 1);

        // Denials on the sync path: capability rule, not sync-enabled, unknown; errors keep the page alive.
        Json denied = sync_call(a, "Probe.secret", "{}");
        ST_CHECK(denied.is_object() && denied["error"]["code"].get_number() == -32003.0);
        denied = sync_call(a, "Math.mul", "{a:2,b:3}");
        ST_CHECK(denied.is_object() && denied["error"]["code"].get_number() == -32005.0);
        denied = sync_call(a, "Nope.method", "{}");
        ST_CHECK(denied.is_object() && denied["error"]["code"].get_number() == -32005.0);
        const Json boom = sync_call(a, "Probe.boom", "{}");
        ST_CHECK(boom.is_object() && boom["status"].get_string() == "error" && contains(boom["error"]["message"].get_string(), "probe exploded"));
        eval(a, "window.__asyncSecret=null;window.__rpc('Probe.secret',{}).then(r=>{window.__asyncSecret=r.error?r.error.code:0;});1");
        ST_CHECK(pump_until([&]() { return eval(a, "window.__asyncSecret").value_or("null") == "-32003"; }, std::chrono::seconds{10}));

        // ElectronIpc.sendSync with the sender id; SyncHost zoom round trip.
        const Json who = sync_call(a, "ElectronIpc.sendSync", "{channel:'whoami',args:[]}");
        ST_CHECK(who.is_object() && static_cast<std::uint64_t>(who["data"].get_number()) == a.id());
        Json zoom = sync_call(a, "SyncHost.set_zoom_factor", "{value:1.25}");
        ST_CHECK(zoom.is_object() && zoom["status"].get_string() == "success");
        ST_CHECK(std::abs(a.zoom_factor().value_or(0.0) - 1.25) < 1e-6);
        zoom = sync_call(a, "SyncHost.zoom_factor", "{}");
        ST_CHECK(zoom.is_object() && std::abs(zoom["data"]["value"].get_number() - 1.25) < 1e-6);
        zoom = sync_call(a, "SyncHost.set_zoom_factor", "{value:99}");
        ST_CHECK(zoom.is_object() && zoom["status"].get_string() == "error");
        sync_call(a, "SyncHost.set_zoom_factor", "{value:1}");

        // webUtils.getPathForFile plumbing: the files message is always handled before the sync call that follows it.
        // WebView2 refuses File objects that are not backed by a file on disk (webUtils.getPathForFile returns '').
        ST_CHECK(contains(eval(a, "(()=>{try{chrome.webview.postMessageWithAdditionalObjects({__boson_sync:'files',token:5000},[new File(['x'],'x.txt')]);"
                                  "return 'posted';}catch(e){return 'threw: '+e.message;}})()").value_or(""), "not a file on the disk"));
        const Json files = eval_json(a, "(()=>{const h=chrome.webview.hostObjects.sync.boson;let found=0,empty=0;"
                                        "for(let i=1;i<=200;i++){chrome.webview.postMessageWithAdditionalObjects({__boson_sync:'files',token:i},[]);"
                                        "const r=JSON.parse(h.invoke('SyncHost.file_paths',JSON.stringify({token:i})));"
                                        "if(r.status==='success'){found++;if(r.data.paths.length===0)empty++;}}"
                                        "const miss=JSON.parse(h.invoke('SyncHost.file_paths',JSON.stringify({token:9999})));"
                                        "return {found,empty,miss:miss.status};})()");
        ST_CHECK(files.is_object() && files["found"].get_number() == 200.0 && files["empty"].get_number() == 200.0);
        ST_CHECK(files.is_object() && files["miss"].get_string() == "error");
        std::cout << "  files message -> sync call ordering: " << (files.is_object() ? files["found"].get_number() : 0.0) << "/200 in order\n";

        // Origin check: after b navigates to a document with an opaque origin its calls are rejected.
        b_nav = false;
        ST_CHECK(b.navigate_to_string("<html><body>other origin</body></html>"));
        ST_CHECK(pump_until([&]() { return b_nav; }));
        const Json foreign = sync_call(b, "Math.add", "{a:1,b:1}");
        ST_CHECK(foreign.is_object() && foreign["error"]["code"].get_number() == -32003.0 &&
                 contains(foreign["error"]["message"].get_string(), "untrusted origin"));
        ST_CHECK(eval(b, "typeof window.__BOSON_BOOT__") == std::optional<std::string>{"\"undefined\""});

        // detach() removes the host object.
        sync->detach(b);
        const Json gone = sync_call(b, "Math.add", "{}");
        ST_CHECK(gone.is_object() && gone.get_object().contains("thrown"));
        ST_CHECK(!sync->is_attached(b.id()));

        const auto& stats = sync->stats();
        std::cout << "  SyncBridge stats: " << stats.calls << " calls, " << stats.rejected << " rejected, " << stats.over_budget
                  << " over budget, mean handler " << (stats.calls != 0 ? stats.total_ns / stats.calls / 1000 : 0) << " us\n";

        bool shut_down = false;
        sync.reset();
        env->shutdown([&]() { shut_down = true; });
        ST_CHECK(pump_until([&]() { return shut_down; }, std::chrono::seconds{10}));
        ipc.set_bridge(nullptr);
        bridge.reset();
        host.reset();
        window.reset();
    }
    std::error_code ec;
    std::filesystem::remove_all(user_data, ec);
}

} // namespace

int main() {
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    test_host_object();
    test_sync_core();
    test_boot_values();
    test_drag_out();
    test_webview();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " sync checks passed\n";
    ::CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
