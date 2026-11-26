#include <boson/shipping/version.hpp>

#include <algorithm>
#include <charconv>

namespace boson::shipping {

namespace {

bool is_digit(char ch) noexcept {
    return ch >= '0' && ch <= '9';
}

bool is_identifier_char(char ch) noexcept {
    return is_digit(ch) || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-';
}

bool all_digits(std::string_view s) noexcept {
    return !s.empty() && std::all_of(s.begin(), s.end(), is_digit);
}

std::optional<std::uint64_t> parse_number(std::string_view s) noexcept {
    if (!all_digits(s)) {
        return std::nullopt;
    }
    // SemVer forbids leading zeroes in numeric components.
    if (s.size() > 1 && s.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

std::strong_ordering compare_identifier(std::string_view a, std::string_view b) noexcept {
    const bool a_num = all_digits(a);
    const bool b_num = all_digits(b);
    if (a_num && b_num) {
        // Numeric identifiers compare numerically; without leading zeroes a longer one is larger.
        if (a.size() != b.size()) {
            return a.size() <=> b.size();
        }
        const int c = a.compare(b);
        return c <=> 0;
    }
    if (a_num != b_num) {
        // Numeric identifiers always have lower precedence than alphanumeric ones.
        return a_num ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    const int c = a.compare(b);
    return c <=> 0;
}

} // namespace

std::optional<SemVer> SemVer::parse(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    SemVer v;
    const std::size_t plus = text.find('+');
    if (plus != std::string_view::npos) {
        const std::string_view build = text.substr(plus + 1);
        if (build.empty()) {
            return std::nullopt;
        }
        for (const auto part : split(build, '.')) {
            if (part.empty() || !std::all_of(part.begin(), part.end(), is_identifier_char)) {
                return std::nullopt;
            }
        }
        v.build = std::string(build);
        text = text.substr(0, plus);
    }

    const std::size_t dash = text.find('-');
    if (dash != std::string_view::npos) {
        const std::string_view pre = text.substr(dash + 1);
        if (pre.empty()) {
            return std::nullopt;
        }
        for (const auto part : split(pre, '.')) {
            if (part.empty() || !std::all_of(part.begin(), part.end(), is_identifier_char)) {
                return std::nullopt;
            }
            if (all_digits(part) && part.size() > 1 && part.front() == '0') {
                return std::nullopt;
            }
            v.prerelease.emplace_back(part);
        }
        text = text.substr(0, dash);
    }

    const auto core = split(text, '.');
    if (core.empty() || core.size() > 4) {
        return std::nullopt;
    }
    std::uint64_t* const fields[4] = {&v.major, &v.minor, &v.patch, &v.revision};
    for (std::size_t i = 0; i < core.size(); ++i) {
        const auto number = parse_number(core[i]);
        if (!number.has_value()) {
            return std::nullopt;
        }
        *fields[i] = *number;
    }
    return v;
}

std::string SemVer::to_string() const {
    std::string out = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (revision != 0) {
        out += "." + std::to_string(revision);
    }
    for (std::size_t i = 0; i < prerelease.size(); ++i) {
        out.push_back(i == 0 ? '-' : '.');
        out += prerelease[i];
    }
    if (!build.empty()) {
        out.push_back('+');
        out += build;
    }
    return out;
}

std::string SemVer::to_four_part() const {
    const auto clamp = [](std::uint64_t value) { return std::to_string(std::min<std::uint64_t>(value, 65535)); };
    return clamp(major) + "." + clamp(minor) + "." + clamp(patch) + "." + clamp(revision);
}

std::strong_ordering SemVer::compare(const SemVer& a, const SemVer& b) noexcept {
    if (const auto c = a.major <=> b.major; c != 0) return c;
    if (const auto c = a.minor <=> b.minor; c != 0) return c;
    if (const auto c = a.patch <=> b.patch; c != 0) return c;
    if (const auto c = a.revision <=> b.revision; c != 0) return c;

    // A release is newer than any of its pre-releases.
    if (a.prerelease.empty() || b.prerelease.empty()) {
        if (a.prerelease.empty() == b.prerelease.empty()) {
            return std::strong_ordering::equal;
        }
        return a.prerelease.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
    }
    const std::size_t n = std::min(a.prerelease.size(), b.prerelease.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (const auto c = compare_identifier(a.prerelease[i], b.prerelease[i]); c != 0) {
            return c;
        }
    }
    return a.prerelease.size() <=> b.prerelease.size();
}

std::optional<std::strong_ordering> compare_versions(std::string_view a, std::string_view b) {
    const auto va = SemVer::parse(a);
    const auto vb = SemVer::parse(b);
    if (!va || !vb) {
        return std::nullopt;
    }
    return SemVer::compare(*va, *vb);
}

bool is_newer_version(std::string_view candidate, std::string_view current) {
    const auto c = compare_versions(candidate, current);
    return c.has_value() && *c == std::strong_ordering::greater;
}

} // namespace boson::shipping
