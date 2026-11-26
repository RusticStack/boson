#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::shipping {

/// @brief A Semantic Versioning 2.0.0 version, relaxed for Windows packaging:
///
/// - an optional leading 'v' / 'V' is accepted ("v1.2.3");
/// - one to four numeric components are accepted ("1", "1.2", "1.2.3", "1.2.3.4"); missing components are 0,
///   so "1.2" == "1.2.0" == "1.2.0.0";
/// - pre-release ("-beta.2") ordering follows SemVer section 11; build metadata ("+abc") is kept but ignored
///   for comparison.
struct SemVer {
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
    std::uint64_t revision = 0;
    std::vector<std::string> prerelease;
    std::string build;

    [[nodiscard]] static std::optional<SemVer> parse(std::string_view text);

    /// @brief "MAJOR.MINOR.PATCH[.REVISION][-pre][+build]" (REVISION only when non-zero).
    [[nodiscard]] std::string to_string() const;
    /// @brief "MAJOR.MINOR.PATCH.REVISION" with each part clamped to 65535 (MSI / MSIX / VERSIONINFO form).
    [[nodiscard]] std::string to_four_part() const;

    [[nodiscard]] bool is_prerelease() const noexcept { return !prerelease.empty(); }

    [[nodiscard]] friend std::strong_ordering operator<=>(const SemVer& a, const SemVer& b) noexcept {
        return compare(a, b);
    }
    [[nodiscard]] friend bool operator==(const SemVer& a, const SemVer& b) noexcept {
        return compare(a, b) == std::strong_ordering::equal;
    }

    [[nodiscard]] static std::strong_ordering compare(const SemVer& a, const SemVer& b) noexcept;
};

/// @brief Compares two version strings; std::nullopt when either does not parse.
[[nodiscard]] std::optional<std::strong_ordering> compare_versions(std::string_view a, std::string_view b);

/// @brief true when candidate parses and is strictly newer than current.
[[nodiscard]] bool is_newer_version(std::string_view candidate, std::string_view current);

} // namespace boson::shipping
