// boson_updater_sign: ECDSA P-256 (Windows CNG) key generation and update-package signing for boson::shipping::Updater.
//
//   boson_updater_sign keygen <private-key-file> [--force]
//   boson_updater_sign pubkey <private-key-file | env:VAR>
//   boson_updater_sign sign <private-key-file | env:VAR> <package> <version>
//                      [--url <url>] [--kind msi|zip|exe] [--notes <text>] [--rollout <0-100>]
//                      [--mandatory] [--minimum-version <v>] [--installer-args <args>] [--manifest <out.json>]
//   boson_updater_sign verify <public-key-base64> <package> <version> <signature-base64>
//   boson_updater_sign hash <file>
#include <windows.h>

#include <boson/core/result.hpp>
#include <boson/shipping/crypto.hpp>
#include <boson/shipping/updater.hpp>
#include <boson/shipping/version.hpp>

#include <glaze/glaze.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using boson::core::wide_to_utf8;
namespace sh = boson::shipping;

int usage() {
    std::cerr << "usage:\n"
                 "  boson_updater_sign keygen <private-key-file> [--force]\n"
                 "  boson_updater_sign pubkey <private-key-file | env:VAR>\n"
                 "  boson_updater_sign sign <private-key-file | env:VAR> <package> <version> [--url <url>] [--kind msi|zip|exe]\n"
                 "                     [--notes <text>] [--rollout <0-100>] [--mandatory] [--minimum-version <v>]\n"
                 "                     [--installer-args <args>] [--manifest <out.json>]\n"
                 "  boson_updater_sign verify <public-key-base64> <package> <version> <signature-base64>\n"
                 "  boson_updater_sign hash <file>\n";
    return 64;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<sh::Bytes> load_private_key(std::wstring_view source) {
    std::string text;
    if (source.starts_with(L"env:")) {
        const std::wstring name(source.substr(4));
        const DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        if (needed == 0) {
            std::cerr << "environment variable " << wide_to_utf8(name) << " is not set\n";
            return std::nullopt;
        }
        std::wstring value(needed, L'\0');
        value.resize(GetEnvironmentVariableW(name.c_str(), value.data(), needed));
        text = wide_to_utf8(value);
    } else {
        auto content = read_file(std::filesystem::path{source});
        if (!content) {
            std::cerr << "cannot read " << wide_to_utf8(source) << "\n";
            return std::nullopt;
        }
        text = std::move(*content);
    }
    auto key = sh::base64_decode(text);
    SecureZeroMemory(text.data(), text.size());
    if (!key || key->size() != sh::kEcdsaP256PrivateKeySize) {
        std::cerr << "not a base64 ECDSA P-256 private key (96 bytes)\n";
        return std::nullopt;
    }
    return key;
}

std::string public_of(const sh::Bytes& private_key) {
    return sh::base64_encode(std::span<const std::uint8_t>(private_key.data(), sh::kEcdsaP256PublicKeySize));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        return usage();
    }
    const std::wstring_view command = argv[1];
    std::vector<std::wstring_view> args(argv + 2, argv + argc);

    if (command == L"keygen" && !args.empty()) {
        const std::filesystem::path out{args[0]};
        const bool force = args.size() > 1 && args[1] == L"--force";
        if (std::filesystem::exists(out) && !force) {
            std::cerr << "refusing to overwrite " << wide_to_utf8(out.wstring()) << " (pass --force)\n";
            return 1;
        }
        auto pair = sh::ecdsa_p256_generate();
        if (!pair) {
            std::cerr << "key generation failed\n";
            return 1;
        }
        {
            std::ofstream file(out, std::ios::binary | std::ios::trunc);
            file << sh::base64_encode(pair->private_key) << "\n";
            if (!file) {
                std::cerr << "cannot write " << wide_to_utf8(out.wstring()) << "\n";
                return 1;
            }
        }
        SecureZeroMemory(pair->private_key.data(), pair->private_key.size());
        std::cout << "private key written to " << wide_to_utf8(out.wstring()) << " (keep it secret, never commit it)\n";
        std::cout << "public_key: " << sh::base64_encode(pair->public_key) << "\n";
        return 0;
    }

    if (command == L"pubkey" && !args.empty()) {
        const auto key = load_private_key(args[0]);
        if (!key) {
            return 1;
        }
        std::cout << public_of(*key) << "\n";
        return 0;
    }

    if (command == L"hash" && !args.empty()) {
        const auto digest = sh::sha256_file(std::filesystem::path{args[0]});
        if (!digest) {
            std::cerr << "cannot hash " << wide_to_utf8(args[0]) << "\n";
            return 1;
        }
        std::cout << sh::to_hex(*digest) << "\n";
        return 0;
    }

    if (command == L"sign" && args.size() >= 3) {
        auto key = load_private_key(args[0]);
        if (!key) {
            return 1;
        }
        const std::filesystem::path package{args[1]};
        const std::string version = wide_to_utf8(args[2]);
        if (!sh::SemVer::parse(version)) {
            std::cerr << "\"" << version << "\" is not a valid version\n";
            return 1;
        }
        sh::UpdateManifest manifest;
        manifest.version = version;
        std::optional<std::filesystem::path> manifest_out;
        for (std::size_t i = 3; i < args.size(); ++i) {
            const auto next = [&]() -> std::string {
                if (i + 1 >= args.size()) {
                    std::cerr << wide_to_utf8(args[i]) << " needs a value\n";
                    std::exit(64);
                }
                return wide_to_utf8(args[++i]);
            };
            if (args[i] == L"--url") manifest.url = next();
            else if (args[i] == L"--kind") manifest.kind = next();
            else if (args[i] == L"--notes") manifest.notes = next();
            else if (args[i] == L"--rollout") manifest.rollout = std::stod(next());
            else if (args[i] == L"--mandatory") manifest.mandatory = true;
            else if (args[i] == L"--minimum-version") manifest.minimum_version = next();
            else if (args[i] == L"--installer-args") manifest.installer_args = next();
            else if (args[i] == L"--manifest") manifest_out = std::filesystem::path{boson::core::utf8_to_wide(next())};
            else {
                std::cerr << "unknown option " << wide_to_utf8(args[i]) << "\n";
                return 64;
            }
        }
        const auto digest = sh::sha256_file(package);
        if (!digest) {
            std::cerr << "cannot hash " << wide_to_utf8(package.wstring()) << "\n";
            return 1;
        }
        const std::string hex = sh::to_hex(*digest);
        const std::string payload = sh::update_signing_payload(version, hex);
        const auto signature = sh::ecdsa_p256_sign(*key, sh::as_bytes(payload));
        const std::string public_key = public_of(*key);
        SecureZeroMemory(key->data(), key->size());
        if (!signature) {
            std::cerr << "signing failed\n";
            return 1;
        }
        manifest.sha256 = hex;
        manifest.signature = sh::base64_encode(*signature);
        manifest.size = std::filesystem::file_size(package);
        if (manifest.url.empty()) {
            manifest.url = wide_to_utf8(package.filename().wstring());
        }
        std::cout << "sha256:     " << hex << "\n";
        std::cout << "signature:  " << manifest.signature << "\n";
        std::cout << "public_key: " << public_key << "\n";
        if (manifest_out) {
            std::string json;
            if (glz::write<glz::opts{.prettify = true}>(manifest, json)) {
                std::cerr << "cannot serialize the manifest\n";
                return 1;
            }
            std::ofstream file(*manifest_out, std::ios::binary | std::ios::trunc);
            file << json << "\n";
            if (!file) {
                std::cerr << "cannot write the manifest\n";
                return 1;
            }
            std::cout << "manifest written to " << wide_to_utf8(manifest_out->wstring()) << "\n";
        }
        return 0;
    }

    if (command == L"verify" && args.size() >= 4) {
        const auto key = sh::base64_decode(wide_to_utf8(args[0]));
        const auto signature = sh::base64_decode(wide_to_utf8(args[3]));
        const auto digest = sh::sha256_file(std::filesystem::path{args[1]});
        if (!key || !signature || !digest) {
            std::cerr << "bad key, signature or file\n";
            return 1;
        }
        const std::string payload = sh::update_signing_payload(wide_to_utf8(args[2]), sh::to_hex(*digest));
        const bool ok = sh::ecdsa_p256_verify(*key, sh::as_bytes(payload), *signature);
        std::cout << (ok ? "OK: signature valid\n" : "FAIL: signature invalid\n");
        return ok ? 0 : 1;
    }

    return usage();
}
