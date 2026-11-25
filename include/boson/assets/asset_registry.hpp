#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#if defined(__has_embed)
#if __has_embed("../../../frontend/dist/index.html")
#define BOSON_USE_CPP26_EMBED 1
#endif
#endif

#if !defined(BOSON_USE_CPP26_EMBED)
#include "boson_embedded_assets.hpp"
#endif

namespace boson::assets {

namespace detail {

[[nodiscard]] constexpr std::string_view normalize_asset_path(std::string_view path) noexcept {
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    if (path.empty()) {
        return "index.html";
    }
    // Strip query string or fragment if present
    if (const auto q = path.find('?'); q != std::string_view::npos) {
        path = path.substr(0, q);
    }
    if (const auto f = path.find('#'); f != std::string_view::npos) {
        path = path.substr(0, f);
    }
    if (path.empty()) {
        return "index.html";
    }
    return path;
}

#if defined(BOSON_USE_CPP26_EMBED)

inline constexpr std::uint8_t kEmbedIndexHtml[] = {
#embed "../../../frontend/dist/index.html"
};
inline constexpr std::uint8_t kEmbedIndexJs[] = {
#embed "../../../frontend/dist/assets/index.js"
};
inline constexpr std::uint8_t kEmbedIndexCss[] = {
#embed "../../../frontend/dist/assets/index.css"
};

inline constexpr std::array<std::pair<std::string_view, std::span<const std::byte>>, 3> kRegistryEntries = {{
    {"index.html", std::as_bytes(std::span{kEmbedIndexHtml})},
    {"assets/index.js", std::as_bytes(std::span{kEmbedIndexJs})},
    {"assets/index.css", std::as_bytes(std::span{kEmbedIndexCss})},
}};

#else

template <std::size_t... Is>
inline auto make_registry_entries(std::index_sequence<Is...>) noexcept {
    return std::array<std::pair<std::string_view, std::span<const std::byte>>, sizeof...(Is)>{{
        std::pair<std::string_view, std::span<const std::byte>>{
            normalize_asset_path(generated::kEmbeddedAssets[Is].path),
            std::as_bytes(generated::kEmbeddedAssets[Is].data),
        }...
    }};
}

inline const auto kRegistryEntries =
    make_registry_entries(std::make_index_sequence<generated::kEmbeddedAssets.size()>{});

#endif

} // namespace detail

class AssetRegistry {
public:
    [[nodiscard]] static std::optional<std::span<const std::byte>> find(std::string_view path) noexcept {
        const auto normalized = detail::normalize_asset_path(path);
        for (const auto& [entry_path, bytes] : detail::kRegistryEntries) {
            if (entry_path == normalized) {
                return bytes;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::span<const std::pair<std::string_view, std::span<const std::byte>>> all() noexcept {
        return detail::kRegistryEntries;
    }
};

} // namespace boson::assets
