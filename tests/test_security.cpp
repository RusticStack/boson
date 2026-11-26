// Security & storage tests: capability policy, origins, CSP / navigation helpers, DPAPI, Credential Manager
// and the scoped file system. The WebView part skips when no WebView2 runtime is installed.
// Everything this test creates (temp folders, credentials) uses unique names and is removed at the end.
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

namespace st {

struct EchoRequest {
    std::string text;
};

struct EchoResult {
    std::string text;
};

struct DataEnvelope {
    std::uint64_t id = 0;
    std::string status;
    boson::ipc::SafeStorageData data;
};

class TestController {
public:
    BOSON_CONTROLLER("Test");

    [[nodiscard]] EchoResult echo(EchoRequest req) const { return EchoResult{.text = req.text + "!"}; }
    [[nodiscard]] EchoResult secret(EchoRequest) const { return EchoResult{.text = "leaked"}; }
};

} // namespace st

template <>
struct glz::meta<st::TestController> {
    using T = st::TestController;
    static constexpr auto value = glz::object("echo", &T::echo, "secret", &T::secret);
};

namespace {

namespace fs = std::filesystem;
namespace sec = boson::security;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_security.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define SEC_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

template <typename S>
    requires std::convertible_to<const S&, std::string_view>
bool contains(const S& haystack, std::string_view needle) {
    return std::string_view(haystack).find(needle) != std::string_view::npos;
}

bool contains(const std::optional<std::string>& haystack, std::string_view needle) {
    return haystack.has_value() && contains(*haystack, needle);
}

std::string utf8(const fs::path& p) {
    return boson::core::wide_to_utf8(p.wstring());
}

// ---- origins ------------------------------------------------------------------------------------------
void test_origins() {
    step("origin parsing and patterns");
    const auto o = sec::parse_origin("HTTPS://User:pw@Example.COM:8443/path?q#f");
    SEC_CHECK(o.scheme == "HTTPS" && o.host == "Example.COM" && o.port == "8443");
    SEC_CHECK(sec::parse_origin("about:blank").empty());
    SEC_CHECK(sec::parse_origin("data:text/html,hi").empty());
    SEC_CHECK(sec::parse_origin("javascript://%0aalert(1)").host == "%0aalert(1)");
    const auto v6 = sec::parse_origin("http://[::1]:5173/");
    SEC_CHECK(v6.host == "::1" && v6.port == "5173");
    // userinfo trick: the real host is after '@'
    SEC_CHECK(sec::parse_origin("https://boson.app@evil.example/").host == "evil.example");
    SEC_CHECK(sec::normalized_origin("HTTP://LocalHost:5173/index.html") == "http://localhost:5173");
    SEC_CHECK(sec::normalized_origin("http://[::1]:80/") == "http://[::1]:80");

    const sec::OriginPattern exact("boson://app/");
    SEC_CHECK(exact.valid());
    SEC_CHECK(exact.matches("boson://app/index.html"));
    SEC_CHECK(exact.matches("BOSON://APP"));
    SEC_CHECK(!exact.matches("boson://app.evil/"));
    SEC_CHECK(!exact.matches("boson://app:81/"));
    SEC_CHECK(!exact.matches("https://app/"));

    const sec::OriginPattern sub("https://*.example.com");
    SEC_CHECK(sub.matches("https://a.example.com/x"));
    SEC_CHECK(sub.matches("https://a.b.example.com"));
    SEC_CHECK(!sub.matches("https://example.com"));
    SEC_CHECK(!sub.matches("https://evilexample.com"));
    SEC_CHECK(!sub.matches("https://example.com.evil.net"));
    SEC_CHECK(!sub.matches("http://a.example.com"));

    const sec::OriginPattern any_port("http://localhost:*");
    SEC_CHECK(any_port.matches("http://localhost:5173/"));
    SEC_CHECK(any_port.matches("http://localhost/"));
    SEC_CHECK(!any_port.matches("http://localhost.evil:5173/"));

    const sec::OriginPattern scheme("mailto:*");
    SEC_CHECK(scheme.matches("mailto:someone@example.com"));
    SEC_CHECK(!scheme.matches("https://x"));
    SEC_CHECK(sec::OriginPattern("*").matches("data:text/html,x"));

    SEC_CHECK(!sec::OriginPattern("https://exa*mple.com").valid());
    SEC_CHECK(!sec::OriginPattern("https://example.com/path").valid());
    SEC_CHECK(!sec::OriginPattern("example.com").valid());
    SEC_CHECK(!sec::OriginPattern("https://*.").valid());

    const auto defaults = sec::default_app_origins(std::string("http://localhost:5173/"));
    SEC_CHECK(defaults.size() == 2 && defaults[0] == "boson://app" && defaults[1] == "http://localhost:5173");
}

// ---- capabilities ---------------------------------------------------------------------------------------
void test_capabilities() {
    step("capability policy matching");
    constexpr std::string_view kJson = R"({
        "default_deny": true,
        "capabilities": [
            {"identifier": "main", "description": "app window", "origins": ["boson://app"],
             "allow": ["Fs.*", "Boson.minimize", "Credential.get*"], "deny": ["Fs.remove"]},
            {"identifier": "dev", "origins": ["http://localhost:*"], "allow": ["*"], "deny": []},
            {"identifier": "everyone", "allow": ["SafeStorage.is_available"]}
        ]
    })";
    auto policy = sec::CapabilityPolicy::from_json(kJson);
    SEC_CHECK(policy.has_value());
    if (!policy) {
        std::cerr << policy.error().message << '\n';
        return;
    }
    using D = sec::PermissionDecision;
    const auto& p = *policy;
    SEC_CHECK(p.check("Fs.read_text", "boson://app/index.html") == D::Allowed);
    SEC_CHECK(p.check("Fs.write_bytes", "boson://app/") == D::Allowed);
    SEC_CHECK(p.check("Fs.remove", "boson://app/") == D::DeniedByRule);
    SEC_CHECK(p.check("Boson.minimize", "boson://app/") == D::Allowed);
    SEC_CHECK(p.check("Boson.close", "boson://app/") == D::NotGranted);
    SEC_CHECK(p.check("Credential.get", "boson://app/") == D::Allowed);
    SEC_CHECK(p.check("Credential.set", "boson://app/") == D::NotGranted);
    SEC_CHECK(p.check("Fsx.read_text", "boson://app/") == D::NotGranted); // "Fs.*" is not a bare prefix
    SEC_CHECK(p.check("Fs.read_text", "https://evil.example/") == D::NotGranted);
    SEC_CHECK(p.check("Fs.read_text", "https://boson://app@evil/") == D::NotGranted);
    SEC_CHECK(p.check("Boson.close", "http://localhost:5173/") == D::Allowed);
    SEC_CHECK(p.check("SafeStorage.is_available", "https://evil.example/") == D::Allowed);
    SEC_CHECK(p.check("nodot", "boson://app/") == D::Malformed);
    SEC_CHECK(p.check(".x", "boson://app/") == D::Malformed);
    SEC_CHECK(p.check("", "boson://app/") == D::Malformed);
    // deny in one capability wins over allow-all in another that also matches the origin
    {
        sec::CapabilityConfig cfg;
        cfg.capabilities.push_back(sec::Capability{.identifier = "all", .allow = {"*"}});
        cfg.capabilities.push_back(sec::Capability{.identifier = "no-fs", .deny = {"Fs.*"}});
        auto both = sec::CapabilityPolicy::compile(cfg);
        SEC_CHECK(both.has_value() && both->check("Fs.read_text", "boson://app") == D::DeniedByRule);
        SEC_CHECK(both.has_value() && both->check("Boson.close", "boson://app") == D::Allowed);
    }
    // default-allow mode
    {
        sec::CapabilityConfig cfg{.default_deny = false};
        cfg.capabilities.push_back(sec::Capability{.identifier = "x", .deny = {"Credential.*"}});
        auto open = sec::CapabilityPolicy::compile(cfg);
        SEC_CHECK(open.has_value() && open->check("Anything.goes", "https://x") == D::Allowed);
        SEC_CHECK(open.has_value() && open->check("Credential.get", "https://x") == D::DeniedByRule);
    }
    SEC_CHECK(sec::CapabilityPolicy{}.check("Fs.read_text", "boson://app") == D::NotGranted);
    SEC_CHECK(sec::CapabilityPolicy::allow_all()->check("Fs.read_text", "x") == D::Allowed);

    SEC_CHECK(!sec::CapabilityPolicy::from_json(R"({"capabilities":[{"identifier":"x","allow":["Fs"]}]})").has_value());
    SEC_CHECK(!sec::CapabilityPolicy::from_json(R"({"capabilities":[{"identifier":"x","allow":["Fs.*x"]}]})").has_value());
    SEC_CHECK(!sec::CapabilityPolicy::from_json(R"({"capabilities":[{"identifier":"x","origins":["evil"],"allow":["*"]}]})").has_value());
    SEC_CHECK(!sec::CapabilityPolicy::from_json(R"({"default_deny":true,"typo":1})").has_value());
    SEC_CHECK(!sec::CapabilityPolicy::from_json("not json").has_value());
    {
        const std::string round = p.to_json();
        auto again = sec::CapabilityPolicy::from_json(round);
        SEC_CHECK(again.has_value() && again->check("Fs.remove", "boson://app") == D::DeniedByRule);
    }

    step("RPC header peek");
    const auto h1 = sec::peek_rpc_header(R"({"params":{"method":"Evil.x","id":9},"id":7,"method":"Fs.read_text"})");
    SEC_CHECK(h1.has_value() && h1->id == 7 && h1->method == "Fs.read_text");
    SEC_CHECK(!sec::peek_rpc_header(R"({"id":1,"method":"Fs\u002eremove"})").has_value());
    SEC_CHECK(!sec::peek_rpc_header(R"({"id":1})").has_value());
    SEC_CHECK(!sec::peek_rpc_header("[1,2]").has_value());
    SEC_CHECK(!sec::peek_rpc_header("{").has_value());
    const std::string env = sec::make_permission_denied_envelope(7, "Fs.remove", D::DeniedByRule);
    SEC_CHECK(contains(env, "\"id\":7") && contains(env, "-32003") && contains(env, "permission denied: Fs.remove"));
}

// ---- headers and navigation helpers ----------------------------------------------------------------------
void test_web_security_pure() {
    step("CSP header building and navigation decisions");
    const std::string csp = sec::default_content_security_policy();
    SEC_CHECK(contains(csp, "default-src 'self'") && contains(csp, "object-src 'none'") && contains(csp, "frame-ancestors 'none'"));
    SEC_CHECK(!contains(csp, " 'unsafe-eval'") && !contains(csp, "'unsafe-inline' 'self'") && !contains(csp, "localhost"));
    const std::string dev = sec::default_content_security_policy(std::string("http://localhost:5173"));
    SEC_CHECK(contains(dev, "http://localhost:5173") && contains(dev, "ws://localhost:5173"));

    sec::SecurityHeaders opts;
    opts.extra_connect_src = {"https://api.example.com", "bad; script-src *"};
    const std::string headers = sec::build_security_headers(opts, std::nullopt);
    SEC_CHECK(contains(headers, "Content-Security-Policy: default-src 'self'"));
    SEC_CHECK(contains(headers, "https://api.example.com") && !contains(headers, "bad; script-src"));
    SEC_CHECK(contains(headers, "\r\nX-Content-Type-Options: nosniff"));
    SEC_CHECK(contains(headers, "\r\nReferrer-Policy: no-referrer"));
    SEC_CHECK(contains(headers, "\r\nPermissions-Policy: camera=()"));
    SEC_CHECK(!headers.starts_with("\r\n") && !headers.ends_with("\r\n"));

    sec::SecurityHeaders off;
    off.csp_enabled = false;
    off.referrer_policy.clear();
    SEC_CHECK(!contains(sec::build_security_headers(off, std::nullopt), "Content-Security-Policy"));
    SEC_CHECK(!contains(sec::build_security_headers(off, std::nullopt), "Referrer-Policy"));
    sec::SecurityHeaders report;
    report.report_only = true;
    SEC_CHECK(contains(sec::build_security_headers(report, std::nullopt), "Content-Security-Policy-Report-Only: "));

    boson::core::WebViewConfig cfg;
    sec::SecurityHeaders injected;
    injected.content_security_policy = "default-src 'none'\r\nSet-Cookie: x=1";
    SEC_CHECK(!sec::apply_security_headers(cfg, injected));
    SEC_CHECK(cfg.asset_response_headers.empty());
    SEC_CHECK(sec::apply_security_headers(cfg));
    SEC_CHECK(contains(cfg.asset_response_headers, "Content-Security-Policy: "));

    sec::NavigationPolicy nav;
    nav.external_origins = {"https://*.example.com", "mailto:*", "file:*"};
    const sec::CompiledNavigationPolicy compiled(nav, std::string("http://localhost:5173"));
    using N = sec::NavigationAction;
    SEC_CHECK(compiled.decide("boson://app/index.html") == N::Allow);
    SEC_CHECK(compiled.decide("http://localhost:5173/x") == N::Allow);
    SEC_CHECK(compiled.decide("about:blank") == N::Allow);
    SEC_CHECK(compiled.decide("https://docs.example.com/") == N::OpenExternal);
    SEC_CHECK(compiled.decide("mailto:a@b.c") == N::OpenExternal);
    SEC_CHECK(compiled.decide("file:///C:/Windows/win.ini") == N::Block); // external scheme not allowed
    SEC_CHECK(compiled.decide("https://evil.net/") == N::Block);
    SEC_CHECK(compiled.decide("javascript:alert(1)") == N::Block);
    SEC_CHECK(compiled.decide("http://localhost:5174/") == N::Block);

    const auto allowed = sec::compile_origin_patterns(std::vector<std::string>{"boson://app"});
    const auto external = sec::compile_origin_patterns(std::vector<std::string>{"https:*"});
    using W = sec::NewWindowAction;
    SEC_CHECK(sec::decide_new_window(allowed, external, W::NavigateSelf, W::Block, "boson://app/x") == W::NavigateSelf);
    SEC_CHECK(sec::decide_new_window(allowed, external, W::AllowDefault, W::Block, "https://x.org/") == W::OpenExternal);
    SEC_CHECK(sec::decide_new_window(allowed, external, W::AllowDefault, W::Block, "http://x.org/") == W::Block);
    SEC_CHECK(!sec::open_external_uri("file:///C:/Windows/System32/calc.exe"));
    SEC_CHECK(!sec::open_external_uri("C:\\Windows\\System32\\calc.exe"));
}

// ---- DPAPI ------------------------------------------------------------------------------------------------
void test_safe_storage() {
    step("base64 and DPAPI safe storage");
    SEC_CHECK(sec::base64_encode(sec::as_bytes("")) == "");
    SEC_CHECK(sec::base64_encode(sec::as_bytes("f")) == "Zg==");
    SEC_CHECK(sec::base64_encode(sec::as_bytes("fo")) == "Zm8=");
    SEC_CHECK(sec::base64_encode(sec::as_bytes("foobar")) == "Zm9vYmFy");
    {
        auto d = sec::base64_decode("Zm9vYg==");
        SEC_CHECK(d.has_value() && std::string(reinterpret_cast<const char*>(d->data()), d->size()) == "foob");
        SEC_CHECK(!sec::base64_decode("Zm9v!").has_value());
        SEC_CHECK(!sec::base64_decode("Z").has_value());
        SEC_CHECK(!sec::base64_decode("Zg=").has_value());
    }

    SEC_CHECK(sec::SafeStorage::is_available());
    const sec::SafeStorage storage;
    const std::string plain = "p\xC3\xA4ssw\xC3\xB6rd \xF0\x9F\x94\x91 secret";
    auto cipher = storage.encrypt_string(plain);
    SEC_CHECK(cipher.has_value() && cipher->size() > plain.size());
    if (cipher) {
        const std::string_view raw(reinterpret_cast<const char*>(cipher->data()), cipher->size());
        SEC_CHECK(!contains(raw, "secret"));
        auto back = storage.decrypt_string(*cipher);
        SEC_CHECK(back.has_value() && *back == plain);

        const sec::SafeStorage salted("app-entropy");
        SEC_CHECK(!salted.decrypt_string(*cipher).has_value()); // entropy mismatch
        auto salted_cipher = salted.encrypt_string(plain);
        SEC_CHECK(salted_cipher.has_value() && salted.decrypt_string(*salted_cipher) == std::optional<std::string>(plain).value());
        SEC_CHECK(salted_cipher.has_value() && !storage.decrypt_string(*salted_cipher).has_value());

        auto tampered = *cipher;
        tampered[tampered.size() / 2] ^= std::byte{0x5A};
        SEC_CHECK(!storage.decrypt(tampered).has_value());
    }
    std::vector<std::byte> binary(1024);
    for (std::size_t i = 0; i < binary.size(); ++i) {
        binary[i] = static_cast<std::byte>(i * 7);
    }
    auto bcipher = storage.encrypt(binary);
    SEC_CHECK(bcipher.has_value());
    SEC_CHECK(bcipher.has_value() && storage.decrypt(*bcipher) == std::optional<std::vector<std::byte>>(binary).value());
    auto empty = storage.encrypt({});
    SEC_CHECK(empty.has_value() && storage.decrypt(*empty).has_value() && storage.decrypt(*empty)->empty());
    SEC_CHECK(!storage.decrypt({}).has_value());
    const sec::SafeStorage machine({}, sec::ProtectionScope::LocalMachine);
    auto mcipher = machine.encrypt_string("machine");
    SEC_CHECK(mcipher.has_value() && machine.decrypt_string(*mcipher) == std::optional<std::string>("machine").value());

    step("SafeStorage controller via router");
    boson::ipc::SafeStorageController controller;
    boson::ipc::Router<boson::ipc::SafeStorageController> router(controller);
    SEC_CHECK(contains(router.dispatch_sync(R"({"id":1,"method":"SafeStorage.is_available","params":{}})"), "\"value\":true"));
    const std::string enc = router.dispatch_sync(R"({"id":2,"method":"SafeStorage.encrypt_string","params":{"text":"hunter2"}})");
    SEC_CHECK(contains(enc, "\"status\":\"success\"") && !contains(enc, "hunter2"));
    st::DataEnvelope parsed{};
    SEC_CHECK(!glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, enc));
    const std::string dec = router.dispatch_sync(
        std::string(R"({"id":3,"method":"SafeStorage.decrypt_string","params":{"data":")") + parsed.data.data + "\"}}");
    SEC_CHECK(contains(dec, "hunter2"));
    SEC_CHECK(contains(router.dispatch_sync(R"({"id":4,"method":"SafeStorage.decrypt_string","params":{"data":"AAAA"}})"), "\"status\":\"error\""));
    const std::string bytes = router.dispatch_sync(R"({"id":5,"method":"SafeStorage.encrypt_bytes","params":{"data":"AAECAw=="}})");
    st::DataEnvelope pb{};
    SEC_CHECK(!glz::read<glz::opts{.error_on_unknown_keys = false}>(pb, bytes));
    SEC_CHECK(contains(router.dispatch_sync(
        std::string(R"({"id":6,"method":"SafeStorage.decrypt_bytes","params":{"data":")") + pb.data.data + "\"}}"), "AAECAw=="));
}

// ---- Credential Manager ---------------------------------------------------------------------------------
void test_credentials() {
    step("Credential Manager set/get/list/delete");
    const std::string prefix = "boson-sectest-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    const sec::CredentialStore store(prefix);
    SEC_CHECK(store.list().has_value() && store.list()->empty());
    SEC_CHECK(store.get("token").has_value() && !store.get("token")->has_value());
    SEC_CHECK(store.set("token", "s3cr\xC3\xABt", "alice").has_value());
    SEC_CHECK(store.set("refresh", "r-token").has_value());
    auto got = store.get("token");
    SEC_CHECK(got.has_value() && got->has_value() && (*got)->secret == "s3cr\xC3\xABt" && (*got)->username == "alice");
    SEC_CHECK(store.set("token", "rotated").has_value());
    got = store.get("token");
    SEC_CHECK(got.has_value() && got->has_value() && (*got)->secret == "rotated");
    auto keys = store.list();
    SEC_CHECK(keys.has_value() && keys->size() == 2);
    // A different prefix sharing the start must not see these keys.
    const sec::CredentialStore other(prefix + "x");
    SEC_CHECK(other.list().has_value() && other.list()->empty());
    SEC_CHECK(!store.set("", "x").has_value());
    SEC_CHECK(!store.set("bad*key", "x").has_value());
    SEC_CHECK(!store.set("big", std::string(sec::CredentialStore::kMaxSecretBytes + 1, 'x')).has_value());
    SEC_CHECK(store.set("max", std::string(sec::CredentialStore::kMaxSecretBytes, 'x')).has_value());

    step("Credential controller via router");
    boson::ipc::CredentialController controller{sec::CredentialStore(prefix)};
    boson::ipc::Router<boson::ipc::CredentialController> router(controller);
    SEC_CHECK(contains(router.dispatch_sync(R"({"id":1,"method":"Credential.set","params":{"key":"js","secret":"from-js"}})"), "success"));
    SEC_CHECK(contains(router.dispatch_sync(R"({"id":2,"method":"Credential.get","params":{"key":"js"}})"), "from-js"));
    SEC_CHECK(contains(router.dispatch_sync(R"({"id":3,"method":"Credential.list","params":{}})"), "\"js\""));
    SEC_CHECK(contains(router.dispatch_sync(R"({"id":4,"method":"Credential.remove","params":{"key":"js"}})"), "\"value\":true"));
    SEC_CHECK(contains(router.dispatch_sync(R"({"id":5,"method":"Credential.get","params":{"key":"js"}})"), "\"found\":false"));

    // Cleanup: every credential with the test prefix.
    if (auto all = store.list(); all.has_value()) {
        for (const auto& key : *all) {
            SEC_CHECK(store.remove(key).value_or(false));
        }
    }
    SEC_CHECK(store.remove("token") == std::optional<bool>(false).value());
    SEC_CHECK(store.list().has_value() && store.list()->empty());
}

// ---- Scoped file system ---------------------------------------------------------------------------------
bool make_junction(const fs::path& link, const fs::path& target) {
    const std::wstring cmd = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul 2>&1";
    return _wsystem(cmd.c_str()) == 0 && fs::exists(link);
}

void test_fs(const fs::path& base) {
    step("lexical path validation");
    using S = sec::FsScope;
    SEC_CHECK(S::normalize_lexically(L"c:/a/./b/../c//d") == std::optional<std::wstring>(L"C:\\a\\c\\d").value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\..\\x").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\a\\..\\..\\x").has_value());
    SEC_CHECK(!S::normalize_lexically(L"\\\\server\\share\\x").has_value());
    SEC_CHECK(!S::normalize_lexically(L"//server/share/x").has_value());
    SEC_CHECK(!S::normalize_lexically(L"\\\\?\\C:\\Windows").has_value());
    SEC_CHECK(!S::normalize_lexically(L"\\\\.\\PhysicalDrive0").has_value());
    SEC_CHECK(!S::normalize_lexically(L"\\??\\C:\\Windows").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:relative").has_value());
    SEC_CHECK(!S::normalize_lexically(L"\\rooted").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\a\\CON").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\a\\nul.txt").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\a\\COM1.log").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\a\\lpt\u00B9").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\a\\conout$").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\a\\file.txt:secret").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\a\\file.txt::$DATA").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\a\\trail.").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\a\\trail ").has_value());
    SEC_CHECK(!S::normalize_lexically(L"C:\\a\\*.txt").has_value());
    SEC_CHECK(!S::normalize_lexically(std::wstring(L"C:\\a\0b", 6)).has_value());
    SEC_CHECK(S::normalize_lexically(L"C:\\a\\console.txt").has_value());
    SEC_CHECK(S::normalize_lexically(L"C:\\a\\com10").has_value());
    SEC_CHECK(S::is_reserved_device_name(L"Aux.tar.gz"));
    SEC_CHECK(!S::is_reserved_device_name(L"auxiliary"));

    const fs::path root = base / L"root";
    const fs::path outside = base / L"root-outside"; // shares the root's name as a prefix
    const fs::path readonly_dir = base / L"ro";
    fs::create_directories(root / L"sub");
    fs::create_directories(outside);
    fs::create_directories(readonly_dir);
    {
        HANDLE h = CreateFileW((outside / L"secret.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w = 0;
        WriteFile(h, "TOP SECRET", 10, &w, nullptr);
        CloseHandle(h);
        h = CreateFileW((readonly_dir / L"r.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        WriteFile(h, "ro", 2, &w, nullptr);
        CloseHandle(h);
    }

    auto scope = std::make_shared<sec::FsScope>();
    auto added = scope->allow(utf8(root), false, true, "test");
    SEC_CHECK(added.has_value());
    SEC_CHECK(scope->allow(utf8(readonly_dir), true).has_value());
    SEC_CHECK(!scope->allow(utf8(base / L"missing")).has_value());
    SEC_CHECK(!scope->allow("\\\\server\\share").has_value());
    SEC_CHECK(scope->roots().size() == 2);
    sec::ScopedFs sfs(scope, 1024 * 1024);
    const std::string r = utf8(root);

    step("scoped fs operations");
    SEC_CHECK(sfs.write_text(r + "\\a.txt", "hello").has_value());
    SEC_CHECK(sfs.read_text(r + "\\a.txt") == std::optional<std::string>("hello").value());
    SEC_CHECK(sfs.append(r + "\\a.txt", sec::as_bytes(" world")).has_value());
    SEC_CHECK(sfs.read_text(r + "/a.txt") == std::optional<std::string>("hello world").value());
    SEC_CHECK(sfs.write_text(r + "\\a.txt", "replaced").has_value());
    SEC_CHECK(sfs.read_text(r + "\\a.txt") == std::optional<std::string>("replaced").value());
    SEC_CHECK(sfs.write_text("sub/rel.txt", "relative").has_value()); // relative to the first root
    SEC_CHECK(fs::exists(root / L"sub" / L"rel.txt"));
    SEC_CHECK(!sfs.write_text(r + "\\new\\deep\\x.txt", "x").has_value());
    SEC_CHECK(sfs.write_text(r + "\\new\\deep\\x.txt", "x", true).has_value());
    const std::byte bin[] = {std::byte{0}, std::byte{0xFF}, std::byte{0x80}};
    SEC_CHECK(sfs.write_bytes(r + "\\b.bin", bin).has_value());
    SEC_CHECK(sfs.read_bytes(r + "\\b.bin").has_value() && sfs.read_bytes(r + "\\b.bin")->size() == 3);
    SEC_CHECK(!sfs.read_text(r + "\\b.bin").has_value()); // not UTF-8
    SEC_CHECK(sfs.exists(r + "\\a.txt") == std::optional<bool>(true).value());
    SEC_CHECK(sfs.exists(r + "\\nope.txt") == std::optional<bool>(false).value());
    auto st = sfs.stat(r + "\\a.txt");
    SEC_CHECK(st.has_value() && st->exists && st->is_file && st->size == 8 && st->modified_ms > 1600000000000);
    auto dst = sfs.stat(r + "\\sub");
    SEC_CHECK(dst.has_value() && dst->is_dir && !dst->is_symlink);
    SEC_CHECK(sfs.mkdir(r + "\\m1\\m2").has_value() && fs::is_directory(root / L"m1" / L"m2"));
    SEC_CHECK(!sfs.mkdir(r + "\\m1", false).has_value()); // exists
    SEC_CHECK(sfs.rename(r + "\\a.txt", r + "\\m1\\a2.txt").has_value());
    SEC_CHECK(fs::exists(root / L"m1" / L"a2.txt") && !fs::exists(root / L"a.txt"));
    SEC_CHECK(sfs.copy(r + "\\m1\\a2.txt", r + "\\copy.txt").has_value());
    SEC_CHECK(!sfs.copy(r + "\\m1\\a2.txt", r + "\\copy.txt").has_value()); // no overwrite
    SEC_CHECK(sfs.copy(r + "\\m1\\a2.txt", r + "\\copy.txt", true).has_value());
    auto listing = sfs.read_dir(r);
    SEC_CHECK(listing.has_value() && listing->size() >= 5);
    if (listing) {
        bool saw_copy = false;
        for (const auto& e : *listing) {
            saw_copy = saw_copy || (e.name == "copy.txt" && e.is_file && e.size == 8);
        }
        SEC_CHECK(saw_copy);
    }
    SEC_CHECK(!sfs.remove(r + "\\m1").has_value()); // not empty, not recursive
    SEC_CHECK(sfs.remove(r + "\\m1", true).has_value() && !fs::exists(root / L"m1"));
    SEC_CHECK(sfs.remove(r + "\\copy.txt").has_value());
    SEC_CHECK(!sfs.remove(r).has_value());          // scope root
    SEC_CHECK(!sfs.remove(r + "\\", true).has_value());
    SEC_CHECK(!sfs.rename(r, r + "-renamed").has_value());
    {
        sec::ScopedFs tiny(scope, 4);
        SEC_CHECK(!tiny.read_bytes(r + "\\sub\\rel.txt").has_value()); // over the read limit
    }

    step("traversal, device and stream attacks");
    const std::string o = utf8(outside);
    SEC_CHECK(!sfs.read_text(o + "\\secret.txt").has_value());
    SEC_CHECK(!sfs.read_text(r + "\\..\\root-outside\\secret.txt").has_value());
    SEC_CHECK(!sfs.read_text(r + "/../root-outside/secret.txt").has_value());
    SEC_CHECK(!sfs.read_text(r + "\\sub\\..\\..\\root-outside\\secret.txt").has_value());
    SEC_CHECK(!sfs.read_text("..\\root-outside\\secret.txt").has_value());
    SEC_CHECK(!sfs.read_text("sub/../../root-outside/secret.txt").has_value());
    SEC_CHECK(!sfs.write_text(r + "-outside\\pwn.txt", "x").has_value());
    SEC_CHECK(!fs::exists(outside / L"pwn.txt"));
    SEC_CHECK(!sfs.read_text("C:\\Windows\\win.ini").has_value());
    SEC_CHECK(!sfs.read_text("\\\\localhost\\C$\\Windows\\win.ini").has_value());
    SEC_CHECK(!sfs.read_text("\\\\?\\" + o + "\\secret.txt").has_value());
    SEC_CHECK(!sfs.read_text("\\\\.\\" + o + "\\secret.txt").has_value());
    SEC_CHECK(!sfs.read_text("\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\Windows\\win.ini").has_value());
    SEC_CHECK(!sfs.write_text(r + "\\sub\\CON", "x").has_value());
    SEC_CHECK(!sfs.write_text(r + "\\NUL.txt", "x").has_value());
    SEC_CHECK(!sfs.write_text(r + "\\sub\\rel.txt:hidden", "x").has_value());
    SEC_CHECK(!sfs.read_text(r + "\\sub\\rel.txt::$DATA").has_value());
    SEC_CHECK(!sfs.read_text(std::string("\xFF\xFE")).has_value());
    SEC_CHECK(!sfs.read_text("").has_value());
    SEC_CHECK(!sfs.copy(o + "\\secret.txt", r + "\\stolen.txt").has_value());
    SEC_CHECK(!sfs.copy(r + "\\sub\\rel.txt", o + "\\planted.txt").has_value());
    SEC_CHECK(!sfs.rename(r + "\\sub\\rel.txt", o + "\\moved.txt").has_value());
    SEC_CHECK(fs::exists(root / L"sub" / L"rel.txt"));
    {
        auto denied = sfs.read_text(o + "\\secret.txt");
        SEC_CHECK(!denied.has_value() && denied.error().code == ERROR_ACCESS_DENIED && contains(denied.error().message, "permission denied"));
    }

    step("read-only scope");
    const std::string ro = utf8(readonly_dir);
    SEC_CHECK(sfs.read_text(ro + "\\r.txt") == std::optional<std::string>("ro").value());
    SEC_CHECK(!sfs.write_text(ro + "\\r.txt", "x").has_value());
    SEC_CHECK(!sfs.append(ro + "\\r.txt", sec::as_bytes("x")).has_value());
    SEC_CHECK(!sfs.remove(ro + "\\r.txt").has_value());
    SEC_CHECK(!sfs.mkdir(ro + "\\d").has_value());
    SEC_CHECK(sfs.read_text(ro + "\\r.txt") == std::optional<std::string>("ro").value());

    step("junction / symlink escape");
    const fs::path junction = root / L"jn";
    if (make_junction(junction, outside)) {
        const std::string j = utf8(junction);
        SEC_CHECK(!sfs.read_text(j + "\\secret.txt").has_value());
        SEC_CHECK(!sfs.read_dir(j).has_value());
        SEC_CHECK(!sfs.write_text(j + "\\planted.txt", "x").has_value());
        SEC_CHECK(!sfs.write_text(j + "\\new\\planted.txt", "x", true).has_value());
        SEC_CHECK(!sfs.mkdir(j + "\\newdir").has_value());
        SEC_CHECK(!sfs.copy(j + "\\secret.txt", r + "\\stolen.txt").has_value());
        SEC_CHECK(!fs::exists(outside / L"planted.txt") && !fs::exists(outside / L"newdir"));
        auto listing2 = sfs.read_dir(r);
        bool link_flag = false;
        if (listing2) {
            for (const auto& e : *listing2) {
                link_flag = link_flag || (e.name == "jn" && e.is_symlink);
            }
        }
        SEC_CHECK(link_flag);
        // Removing the link itself is allowed and must not touch the target.
        SEC_CHECK(sfs.remove(j, true).has_value());
        SEC_CHECK(!fs::exists(junction) && fs::exists(outside / L"secret.txt"));
    } else {
        std::cout << "  (mklink /J unavailable: junction checks skipped)\n";
    }
    {
        const fs::path link = root / L"slink.txt";
        if (CreateSymbolicLinkW(link.c_str(), (outside / L"secret.txt").c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            SEC_CHECK(!sfs.read_text(utf8(link)).has_value());
            SEC_CHECK(!sfs.write_text(utf8(link), "overwrite").has_value());
            SEC_CHECK(sfs.remove(utf8(link)).has_value());
            SEC_CHECK(fs::exists(outside / L"secret.txt"));
        } else {
            std::cout << "  (symlink creation not permitted: file symlink checks skipped)\n";
        }
    }

    step("non-recursive and single-file scopes");
    {
        auto narrow = std::make_shared<sec::FsScope>();
        SEC_CHECK(narrow->allow(utf8(root), false, false).has_value());
        sec::ScopedFs nfs(narrow);
        SEC_CHECK(nfs.read_text(r + "\\new\\deep\\x.txt").has_value() == false);
        SEC_CHECK(nfs.exists(r + "\\b.bin") == std::optional<bool>(true).value());
        auto single = std::make_shared<sec::FsScope>();
        SEC_CHECK(single->allow(utf8(root / L"b.bin")).has_value());
        sec::ScopedFs one(single);
        SEC_CHECK(one.read_bytes(r + "\\b.bin").has_value());
        SEC_CHECK(!one.read_text(r + "\\sub\\rel.txt").has_value());
        SEC_CHECK(!one.write_text(r + "\\b.bin.x", "x").has_value());
        SEC_CHECK(!one.remove(r + "\\b.bin").has_value()); // the granted file is a root
    }

    step("directory watching");
    {
        std::mutex mutex;
        std::vector<sec::FsChangeBatch> batches;
        auto id = sfs.watch(r, true, [&](sec::FsChangeBatch batch) {
            std::scoped_lock lock(mutex);
            batches.push_back(std::move(batch));
        });
        SEC_CHECK(id.has_value() && sfs.watch_count() == 1);
        SEC_CHECK(!sfs.watch(o, true, [](sec::FsChangeBatch) {}).has_value());
        SEC_CHECK(!sfs.watch(r + "\\b.bin", true, [](sec::FsChangeBatch) {}).has_value());
        SEC_CHECK(sfs.write_text(r + "\\sub\\watched.txt", "1").has_value());
        bool seen = false;
        for (int i = 0; i < 100 && !seen; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            std::scoped_lock lock(mutex);
            for (const auto& b : batches) {
                for (const auto& c : b.changes) {
                    seen = seen || (contains(c.path, "watched.txt") && b.id == *id);
                }
            }
        }
        SEC_CHECK(seen);
        SEC_CHECK(id.has_value() && sfs.unwatch(*id));
        SEC_CHECK(sfs.watch_count() == 0);
        SEC_CHECK(!sfs.unwatch(12345));
        std::size_t before = 0;
        {
            std::scoped_lock lock(mutex);
            before = batches.size();
        }
        SEC_CHECK(sfs.write_text(r + "\\sub\\after.txt", "2").has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        std::scoped_lock lock(mutex);
        SEC_CHECK(batches.size() == before);
    }

    step("Fs controller via router");
    {
        auto shared_fs = std::make_shared<sec::ScopedFs>(scope);
        boson::ipc::FsController controller(shared_fs);
        boson::ipc::Router<boson::ipc::FsController> router(controller);
        const auto json_path = [](const std::string& p) {
            std::string out;
            (void)glz::write_json(p, out);
            return out;
        };
        const std::string jr = json_path(r + "\\ctl.txt");
        SEC_CHECK(contains(router.dispatch_sync(R"({"id":1,"method":"Fs.write_text","params":{"path":)" + jr + R"(,"text":"ctl"}})"), "success"));
        SEC_CHECK(contains(router.dispatch_sync(R"({"id":2,"method":"Fs.read_text","params":{"path":)" + jr + "}}"), "\"text\":\"ctl\""));
        SEC_CHECK(contains(router.dispatch_sync(R"({"id":3,"method":"Fs.read_bytes","params":{"path":)" + jr + "}}"), "\"data\":\"Y3Rs\""));
        SEC_CHECK(contains(router.dispatch_sync(R"({"id":4,"method":"Fs.stat","params":{"path":)" + jr + "}}"), "\"size\":3"));
        SEC_CHECK(contains(router.dispatch_sync(R"({"id":5,"method":"Fs.scopes","params":{}})"), "\"label\":\"test\""));
        const std::string bad = router.dispatch_sync(
            R"({"id":6,"method":"Fs.read_text","params":{"path":)" + json_path(r + "\\..\\root-outside\\secret.txt") + "}}");
        SEC_CHECK(contains(bad, "\"status\":\"error\"") && contains(bad, "permission denied") && !contains(bad, "TOP SECRET"));
        SEC_CHECK(contains(router.dispatch_sync(R"({"id":7,"method":"Fs.grant_from_dialog","params":{}})"), "no dialog owner"));
        std::mutex mutex;
        bool event_seen = false;
        set_fs_event_handler(controller, [&](const sec::FsChangeBatch& b) {
            std::scoped_lock lock(mutex);
            for (const auto& c : b.changes) {
                event_seen = event_seen || contains(c.path, "evt.txt");
            }
        });
        const std::string watch = router.dispatch_sync(R"({"id":8,"method":"Fs.watch","params":{"path":)" + json_path(r) + "}}");
        SEC_CHECK(contains(watch, "\"id\":8") && contains(watch, "success"));
        SEC_CHECK(sfs.write_text(r + "\\evt.txt", "e").has_value());
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            std::scoped_lock lock(mutex);
            if (event_seen) {
                break;
            }
        }
        {
            std::scoped_lock lock(mutex);
            SEC_CHECK(event_seen);
        }
        shared_fs->unwatch_all();
    }
}

// ---- WebView: CSP headers, bridge capability enforcement, navigation policy ---------------------------
bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
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
    pump_until([&]() { return done; }, std::chrono::seconds{10});
    return result;
}

std::optional<std::string> eval_until(boson::core::WebView& webview, std::string_view js, std::string_view not_value = "null") {
    std::optional<std::string> value;
    pump_until([&]() {
        value = eval(webview, js);
        return value.has_value() && *value != not_value;
    }, std::chrono::seconds{10});
    return value;
}

const std::string kCspPage =
    "<!doctype html><html><head><title>CSP</title><script>window.__inline = 1;</script></head><body>csp</body></html>";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/index.html") {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(kCspPage.data()), kCspPage.size());
    }
    return std::nullopt;
}

void test_webview(const fs::path& base) {
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
        std::cout << "SKIP: no WebView2 runtime installed (CSP / bridge checks not run)\n";
        return;
    }
    CoTaskMemFree(version);

    step("WebView: CSP headers, capability enforcement, navigation policy");
    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson security test";
    window_cfg.width = 640;
    window_cfg.height = 480;
    window_cfg.quit_on_destroy = false;
    auto window = std::make_unique<boson::core::Window>(window_cfg);
    SEC_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
    window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = (base / L"udf").wstring();
    const auto env = boson::core::Environment::create(env_cfg);
    auto host = std::make_unique<boson::core::WebViewHost>(*window, env);
    host->set_default_asset_lookup(&lookup);

    boson::core::WebViewConfig cfg;
    cfg.initial_url = "boson://app/index.html";
    sec::SecurityHeaders headers;
    headers.extra_connect_src = {"https://api.example.com"};
    SEC_CHECK(sec::apply_security_headers(cfg, headers));
    bool nav_done = false;
    boson::core::WebView& wv = host->create(cfg);
    wv.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { nav_done = e.is_success; };
    SEC_CHECK(pump_until([&]() { return wv.is_ready() && nav_done; }));

    // execute_script is not subject to the page CSP, so it can observe what the CSP did.
    SEC_CHECK(eval(wv, "typeof window.__inline") == std::optional<std::string>("\"undefined\""));
    SEC_CHECK(eval(wv, "document.title") == std::optional<std::string>("\"CSP\""));
    SEC_CHECK(eval(wv,
        "window.__h = null; fetch('boson://app/index.html').then(r => { window.__h = {"
        "csp: r.headers.get('content-security-policy'), nosniff: r.headers.get('x-content-type-options'),"
        "ref: r.headers.get('referrer-policy'), perm: r.headers.get('permissions-policy'), status: r.status}; })"
        ".catch(e => { window.__h = {error: String(e)}; }); 1").has_value());
    const auto h = eval_until(wv, "window.__h && JSON.stringify(window.__h)");
    std::cout << "  fetched headers: " << h.value_or("<none>") << '\n';
    SEC_CHECK(contains(h, "default-src 'self'"));
    SEC_CHECK(contains(h, "https://api.example.com"));
    SEC_CHECK(contains(h, "nosniff"));
    SEC_CHECK(contains(h, "no-referrer"));
    SEC_CHECK(contains(h, "camera=()"));
    SEC_CHECK(eval(wv,
        "window.__miss = null; fetch('boson://app/missing.js').then(r => { window.__miss = r.status + ' ' +"
        " r.headers.get('x-content-type-options'); }); 1").has_value());
    SEC_CHECK(contains(eval_until(wv, "window.__miss"), "404 nosniff"));
    // connect-src blocks a non-allowed origin before any network access.
    SEC_CHECK(eval(wv, "window.__blocked = null; fetch('https://not-allowed.invalid/').then(() => { window.__blocked = 'fetched'; })"
                       ".catch(e => { window.__blocked = 'blocked'; }); 1").has_value());
    SEC_CHECK(eval_until(wv, "window.__blocked") == std::optional<std::string>("\"blocked\""));

    // ---- bridge capability enforcement ----
    st::TestController test_controller;
    boson::ipc::Router<st::TestController> router(test_controller);
    boson::ipc::Bridge bridge(runner, marshal, router);
    sec::CapabilityConfig caps;
    caps.capabilities.push_back(sec::Capability{.identifier = "main", .origins = {"boson://app"}, .allow = {"Test.echo"}});
    auto policy = sec::CapabilityPolicy::make_shared(caps);
    SEC_CHECK(policy.has_value());
    boson::ipc::BridgeAttachOptions attach_opts;
    attach_opts.capabilities = policy.value_or(nullptr);
    bridge.attach(wv, attach_opts);
    SEC_CHECK(eval(wv, "window.__r = []; chrome.webview.addEventListener('message', e => window.__r.push(e.data)); 1").has_value());
    SEC_CHECK(eval(wv, "chrome.webview.postMessage({id: 1, method: 'Test.echo', params: {text: 'ok'}}); "
                       "chrome.webview.postMessage({id: 2, method: 'Test.secret', params: {text: 'x'}}); "
                       "chrome.webview.postMessage({id: 3, method: 'Nope.thing', params: {}}); 1").has_value());
    SEC_CHECK(pump_until([&]() { return eval(wv, "window.__r.length") == std::optional<std::string>("3"); }, std::chrono::seconds{10}));
    const auto r1 = eval(wv, "JSON.stringify(window.__r.find(x => x.id === 1))");
    const auto r2 = eval(wv, "JSON.stringify(window.__r.find(x => x.id === 2))");
    const auto r3 = eval(wv, "JSON.stringify(window.__r.find(x => x.id === 3))");
    SEC_CHECK(contains(r1, "ok!") && contains(r1, "success"));
    SEC_CHECK(contains(r2, "-32003") && contains(r2, "permission denied: Test.secret") && !contains(r2, "leaked"));
    SEC_CHECK(contains(r3, "-32003"));
    SEC_CHECK(bridge.set_capabilities(wv.id(), sec::CapabilityPolicy::allow_all()));
    SEC_CHECK(eval(wv, "window.__r = []; chrome.webview.postMessage({id: 4, method: 'Test.secret', params: {text: 'x'}}); 1").has_value());
    SEC_CHECK(pump_until([&]() { return eval(wv, "window.__r.length") == std::optional<std::string>("1"); }, std::chrono::seconds{10}));
    SEC_CHECK(contains(eval(wv, "JSON.stringify(window.__r[0])"), "leaked"));

    // ---- navigation / new-window policy (open_external captured: no browser is launched) ----
    std::vector<std::string> external;
    std::vector<std::string> rejected;
    sec::NavigationPolicy nav;
    nav.external_origins = {"https://*.example.com"};
    nav.open_external = [&](std::string_view uri) { external.emplace_back(uri); };
    nav.on_rejected = [&](std::string_view uri, sec::NavigationAction) { rejected.emplace_back(uri); };
    bool chained = false;
    wv.events().on_navigation_starting = [&](boson::core::NavigationStartingEvent&) { chained = true; };
    sec::install_navigation_policy(wv, nav);
    wv.navigate("https://docs.example.com/page");
    SEC_CHECK(pump_until([&]() { return !external.empty(); }, std::chrono::seconds{5}));
    SEC_CHECK(!external.empty() && contains(external.front(), "docs.example.com"));
    wv.navigate("https://evil.invalid/");
    SEC_CHECK(pump_until([&]() { return rejected.size() >= 2; }, std::chrono::seconds{5}));
    SEC_CHECK(external.size() == 1);
    SEC_CHECK(!chained); // the chained handler only sees allowed navigations
    nav_done = false;
    wv.navigate("boson://app/index.html");
    SEC_CHECK(pump_until([&]() { return nav_done; }, std::chrono::seconds{10}));
    SEC_CHECK(chained);
    SEC_CHECK(contains(wv.source(), "boson://app/index.html"));

    std::vector<std::string> popups_external;
    bool popup_blocked = false;
    sec::NewWindowPolicy nw;
    nw.open_external = [&](std::string_view uri) { popups_external.emplace_back(uri); };
    nw.on_rejected = [&](std::string_view, sec::NewWindowAction a) { popup_blocked = popup_blocked || a == sec::NewWindowAction::Block; };
    sec::install_new_window_policy(wv, nw);
    SEC_CHECK(eval(wv, "window.open('https://example.org/'); window.open('ftp://example.org/'); 1").has_value());
    SEC_CHECK(pump_until([&]() { return !popups_external.empty() && popup_blocked; }, std::chrono::seconds{5}));
    SEC_CHECK(!popups_external.empty() && contains(popups_external.front(), "example.org"));

    bridge.detach(wv);
    bool shut = false;
    env->shutdown([&]() { shut = true; });
    pump_until([&]() { return shut; }, std::chrono::seconds{10});
    host.reset();
    window.reset();
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    const fs::path base = fs::temp_directory_path() /
        (L"boson-sec-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(base);

    test_origins();
    test_capabilities();
    test_web_security_pure();
    test_safe_storage();
    test_credentials();
    test_fs(base);
    test_webview(base);

    std::error_code ec;
    for (int i = 0; i < 20; ++i) {
        fs::remove_all(base, ec);
        if (!fs::exists(base)) {
            break;
        }
        Sleep(250); // WebView2 may still hold the user data folder briefly
    }
    SEC_CHECK(!fs::exists(base));

    std::cout << (g_checks - g_failures) << "/" << g_checks << " security checks passed\n";
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
