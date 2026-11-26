#include <boson/archive/zip.hpp>

#include <boson/core/result.hpp>

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace boson::archive {

namespace {

constexpr std::uint32_t kLocalSig = 0x04034b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kEocdSig = 0x06054b50;
constexpr std::uint32_t kEocd64Sig = 0x06064b50;
constexpr std::uint32_t kEocd64LocatorSig = 0x07064b50;
constexpr std::size_t kEocdSize = 22;
constexpr std::size_t kCentralSize = 46;
constexpr std::size_t kLocalSize = 30;
constexpr std::size_t kEocd64LocatorSize = 20;
constexpr std::size_t kEocd64Size = 56;

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

UniqueHandle make_handle(HANDLE handle) noexcept {
    return UniqueHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

struct ViewUnmapper {
    void operator()(const void* view) const noexcept {
        if (view != nullptr) {
            UnmapViewOfFile(view);
        }
    }
};

ZipError make_error(ZipErrorKind kind, std::string message, std::string entry = {}, std::uint32_t win32 = 0) {
    return ZipError{.kind = kind, .message = std::move(message), .entry = std::move(entry), .win32_error = win32};
}

std::uint16_t rd16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(p[0]) | (std::to_integer<unsigned>(p[1]) << 8));
}
std::uint32_t rd32(const std::byte* p) noexcept {
    return static_cast<std::uint32_t>(rd16(p)) | (static_cast<std::uint32_t>(rd16(p + 2)) << 16);
}
std::uint64_t rd64(const std::byte* p) noexcept {
    return static_cast<std::uint64_t>(rd32(p)) | (static_cast<std::uint64_t>(rd32(p + 4)) << 32);
}

void wr16(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}
void wr32(std::vector<std::byte>& out, std::uint32_t v) {
    wr16(out, v & 0xFFFF);
    wr16(out, v >> 16);
}
void wr64(std::vector<std::byte>& out, std::uint64_t v) {
    wr32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
    wr32(out, static_cast<std::uint32_t>(v >> 32));
}
void wr_bytes(std::vector<std::byte>& out, std::string_view s) {
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
}

bool is_reserved_device_name(std::wstring_view component) {
    // Windows treats "CON", "con.txt", "NUL .log" ... as devices: compare the part before the first '.'.
    std::wstring_view base = component.substr(0, component.find(L'.'));
    while (!base.empty() && base.back() == L' ') {
        base.remove_suffix(1);
    }
    if (base.size() != 3 && base.size() != 4) {
        return false;
    }
    std::wstring upper(base);
    for (auto& ch : upper) {
        if (ch >= L'a' && ch <= L'z') {
            ch = static_cast<wchar_t>(ch - L'a' + L'A');
        }
    }
    static constexpr std::array<std::wstring_view, 6> kThree{L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    for (const auto name : kThree) {
        if (upper == name) {
            return true;
        }
    }
    if (upper.size() == 4 && (upper.starts_with(L"COM") || upper.starts_with(L"LPT"))) {
        const wchar_t digit = upper[3];
        return (digit >= L'0' && digit <= L'9') || digit == L'\u00B9' || digit == L'\u00B2' || digit == L'\u00B3';
    }
    return false;
}

bool valid_utf8(std::string_view text) noexcept {
    return text.empty() ||
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}

/// Entry names without the UTF-8 flag are CP437 (the ZIP default); convert those to UTF-8.
std::string decode_name(std::string_view raw, bool utf8_flag) {
    bool ascii = true;
    for (const char c : raw) {
        ascii = ascii && static_cast<unsigned char>(c) < 0x80;
    }
    if (ascii || utf8_flag) {
        return std::string(raw);
    }
    const int needed = MultiByteToWideChar(437, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (needed <= 0) {
        return std::string(raw);
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(437, 0, raw.data(), static_cast<int>(raw.size()), wide.data(), needed);
    return core::wide_to_utf8(wide);
}

std::wstring upper_key(const std::filesystem::path& p) {
    std::wstring key = p.native();
    if (!key.empty()) {
        CharUpperBuffW(key.data(), static_cast<DWORD>(key.size()));
    }
    return key;
}

/// "\\?\C:\..." form so extraction is not limited to MAX_PATH.
std::wstring extended(const std::filesystem::path& absolute) {
    const std::wstring& s = absolute.native();
    if (s.starts_with(L"\\\\?\\")) {
        return s;
    }
    if (s.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + s.substr(2);
    }
    return L"\\\\?\\" + s;
}

std::optional<std::wstring> final_path(HANDLE handle) {
    std::wstring buf(512, L'\0');
    DWORD len = GetFinalPathNameByHandleW(handle, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len >= buf.size()) {
        buf.resize(static_cast<std::size_t>(len) + 1);
        len = GetFinalPathNameByHandleW(handle, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    if (len == 0 || len >= buf.size()) {
        return std::nullopt;
    }
    buf.resize(len);
    return buf;
}

bool path_within(std::wstring_view child, std::wstring_view root) {
    if (child.size() <= root.size()) {
        return false;
    }
    if (CompareStringOrdinal(child.data(), static_cast<int>(root.size()), root.data(), static_cast<int>(root.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    return root.ends_with(L'\\') || child[root.size()] == L'\\';
}

FILETIME dos_to_filetime(std::uint16_t date, std::uint16_t time) noexcept {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(1980 + ((date >> 9) & 0x7F));
    st.wMonth = static_cast<WORD>(std::clamp((date >> 5) & 0x0F, 1, 12));
    st.wDay = static_cast<WORD>(std::clamp(date & 0x1F, 1, 31));
    st.wHour = static_cast<WORD>(std::min((time >> 11) & 0x1F, 23));
    st.wMinute = static_cast<WORD>(std::min((time >> 5) & 0x3F, 59));
    st.wSecond = static_cast<WORD>(std::min((time & 0x1F) * 2, 59));
    FILETIME local{};
    FILETIME utc{};
    if (SystemTimeToFileTime(&st, &local) && LocalFileTimeToFileTime(&local, &utc)) {
        return utc;
    }
    return FILETIME{};
}

} // namespace

std::string_view to_string(ZipErrorKind kind) noexcept {
    switch (kind) {
    case ZipErrorKind::Io: return "io";
    case ZipErrorKind::NotZip: return "not-zip";
    case ZipErrorKind::Corrupt: return "corrupt";
    case ZipErrorKind::Unsupported: return "unsupported";
    case ZipErrorKind::UnsafePath: return "unsafe-path";
    case ZipErrorKind::Symlink: return "symlink";
    case ZipErrorKind::Duplicate: return "duplicate";
    case ZipErrorKind::LimitExceeded: return "limit-exceeded";
    case ZipErrorKind::CrcMismatch: return "crc-mismatch";
    case ZipErrorKind::Cancelled: return "cancelled";
    }
    return "unknown";
}

std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t previous) noexcept {
    std::uint32_t crc = previous;
    const auto* ptr = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t left = data.size();
    while (left > 0) {
        const std::size_t chunk = std::min<std::size_t>(left, 1u << 30);
        crc = static_cast<std::uint32_t>(mz_crc32(crc, ptr, chunk));
        ptr += chunk;
        left -= chunk;
    }
    return crc;
}

ZipResult<std::filesystem::path> sanitize_entry_name(std::string_view name, const ZipLimits& limits) {
    const std::string entry(name);
    if (name.empty()) {
        return std::unexpected(make_error(ZipErrorKind::UnsafePath, "empty entry name", entry));
    }
    if (name.size() > limits.max_name_bytes) {
        return std::unexpected(make_error(ZipErrorKind::LimitExceeded, "entry name too long", entry));
    }
    if (!valid_utf8(name)) {
        return std::unexpected(make_error(ZipErrorKind::UnsafePath, "entry name is not valid UTF-8", entry));
    }
    if (name.front() == '/' || name.front() == '\\') {
        return std::unexpected(make_error(ZipErrorKind::UnsafePath, "absolute entry path", entry));
    }
    std::filesystem::path out;
    std::size_t depth = 0;
    std::size_t start = 0;
    while (start <= name.size()) {
        std::size_t end = name.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = name.size();
        }
        const std::string_view part = name.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") {
            if (end == name.size()) {
                break;
            }
            continue;
        }
        if (part == "..") {
            return std::unexpected(make_error(ZipErrorKind::UnsafePath, "'..' in entry path", entry));
        }
        for (const char c : part) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
                return std::unexpected(make_error(
                    ZipErrorKind::UnsafePath,
                    c == ':' ? "drive prefix or alternate data stream in entry path" : "invalid character in entry path",
                    entry));
            }
        }
        if (part.back() == '.' || part.back() == ' ') {
            return std::unexpected(make_error(ZipErrorKind::UnsafePath, "path component ends with '.' or ' '", entry));
        }
        const std::wstring wide = core::utf8_to_wide(part);
        if (wide.empty() || is_reserved_device_name(wide)) {
            return std::unexpected(make_error(ZipErrorKind::UnsafePath, "reserved device name in entry path", entry));
        }
        if (++depth > limits.max_depth) {
            return std::unexpected(make_error(ZipErrorKind::LimitExceeded, "entry path too deep", entry));
        }
        out /= wide;
        if (end == name.size()) {
            break;
        }
    }
    return out;
}

struct ZipReader::Impl {
    UniqueHandle file;
    UniqueHandle mapping;
    std::unique_ptr<const void, ViewUnmapper> view;
    std::span<const std::byte> data;
    std::vector<ZipEntry> entries;
    std::unordered_map<std::string_view, std::size_t> by_name;
    ZipLimits limits;
    std::uint64_t total = 0;
    std::uint64_t central_offset = 0;

    ZipResult<void> parse();
    ZipResult<std::uint64_t> data_offset(const ZipEntry& entry) const;
};

ZipResult<void> ZipReader::Impl::parse() {
    const std::size_t size = data.size();
    if (size < kEocdSize) {
        return std::unexpected(make_error(ZipErrorKind::NotZip, "file too small to be a ZIP archive"));
    }
    // End of central directory: last 22 + up to 65535 comment bytes.
    const std::size_t search_floor = size > kEocdSize + 0xFFFF ? size - kEocdSize - 0xFFFF : 0;
    std::size_t eocd = std::numeric_limits<std::size_t>::max();
    for (std::size_t pos = size - kEocdSize + 1; pos-- > search_floor;) {
        if (rd32(data.data() + pos) == kEocdSig) {
            const std::uint16_t comment = rd16(data.data() + pos + 20);
            if (pos + kEocdSize + comment <= size) {
                eocd = pos;
                break;
            }
        }
    }
    if (eocd == std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(make_error(ZipErrorKind::NotZip, "no end-of-central-directory record"));
    }
    const std::byte* e = data.data() + eocd;
    std::uint64_t disk = rd16(e + 4);
    std::uint64_t cd_disk = rd16(e + 6);
    std::uint64_t count = rd16(e + 10);
    std::uint64_t cd_size = rd32(e + 12);
    std::uint64_t cd_offset = rd32(e + 16);

    const bool maybe64 = count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu;
    if (eocd >= kEocd64LocatorSize && rd32(data.data() + eocd - kEocd64LocatorSize) == kEocd64LocatorSig) {
        const std::byte* loc = data.data() + eocd - kEocd64LocatorSize;
        const std::uint64_t eocd64 = rd64(loc + 8);
        if (eocd64 > size || size - eocd64 < kEocd64Size || rd32(data.data() + eocd64) != kEocd64Sig) {
            return std::unexpected(make_error(ZipErrorKind::Corrupt, "bad ZIP64 end-of-central-directory record"));
        }
        const std::byte* z = data.data() + eocd64;
        disk = rd32(z + 16);
        cd_disk = rd32(z + 20);
        count = rd64(z + 32);
        cd_size = rd64(z + 40);
        cd_offset = rd64(z + 48);
    } else if (maybe64) {
        return std::unexpected(make_error(ZipErrorKind::Corrupt, "ZIP64 sizes without a ZIP64 locator"));
    }
    if (disk != 0 || cd_disk != 0) {
        return std::unexpected(make_error(ZipErrorKind::Unsupported, "multi-disk archives are not supported"));
    }
    if (count > limits.max_entries) {
        return std::unexpected(make_error(ZipErrorKind::LimitExceeded, "too many entries (" + std::to_string(count) + ")"));
    }
    if (cd_offset > size || cd_size > size - cd_offset) {
        return std::unexpected(make_error(ZipErrorKind::Corrupt, "central directory outside the file"));
    }
    central_offset = cd_offset;
    // Every central record is >= 46 bytes: a count the directory cannot hold is corrupt (and must not
    // drive a huge reserve()).
    if (count > cd_size / kCentralSize) {
        return std::unexpected(make_error(ZipErrorKind::Corrupt, "entry count larger than the central directory"));
    }
    entries.reserve(static_cast<std::size_t>(count));

    std::unordered_set<std::wstring> seen;
    seen.reserve(static_cast<std::size_t>(count));
    std::uint64_t pos = cd_offset;
    const std::uint64_t cd_end = cd_offset + cd_size;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (cd_end - pos < kCentralSize || rd32(data.data() + pos) != kCentralSig) {
            return std::unexpected(make_error(ZipErrorKind::Corrupt, "bad central directory entry #" + std::to_string(i)));
        }
        const std::byte* c = data.data() + pos;
        ZipEntry entry;
        const std::uint16_t made_by = rd16(c + 4);
        entry.host_os = static_cast<std::uint8_t>(made_by >> 8);
        entry.flags = rd16(c + 8);
        entry.method = rd16(c + 10);
        entry.dos_time = rd16(c + 12);
        entry.dos_date = rd16(c + 14);
        entry.crc32 = rd32(c + 16);
        entry.compressed_size = rd32(c + 20);
        entry.uncompressed_size = rd32(c + 24);
        const std::uint16_t name_len = rd16(c + 28);
        const std::uint16_t extra_len = rd16(c + 30);
        const std::uint16_t comment_len = rd16(c + 32);
        std::uint64_t disk_start = rd16(c + 34);
        entry.external_attributes = rd32(c + 38);
        entry.local_header_offset = rd32(c + 42);
        const std::uint64_t record = kCentralSize + static_cast<std::uint64_t>(name_len) + extra_len + comment_len;
        if (cd_end - pos < record) {
            return std::unexpected(make_error(ZipErrorKind::Corrupt, "central directory entry overruns the directory"));
        }
        const std::string_view raw_name(reinterpret_cast<const char*>(c + kCentralSize), name_len);
        entry.name = decode_name(raw_name, (entry.flags & 0x0800) != 0);
        std::replace(entry.name.begin(), entry.name.end(), '\\', '/');

        // ZIP64 extended information.
        const std::byte* extra = c + kCentralSize + name_len;
        std::size_t off = 0;
        while (off + 4 <= extra_len) {
            const std::uint16_t id = rd16(extra + off);
            const std::uint16_t len = rd16(extra + off + 2);
            if (off + 4 + len > extra_len) {
                return std::unexpected(make_error(ZipErrorKind::Corrupt, "bad extra field", entry.name));
            }
            if (id == 0x0001) {
                entry.zip64 = true;
                const std::byte* z = extra + off + 4;
                std::size_t zl = 0;
                const auto take = [&](std::uint64_t& field) -> bool {
                    if (zl + 8 > len) {
                        return false;
                    }
                    field = rd64(z + zl);
                    zl += 8;
                    return true;
                };
                bool ok = true;
                if (entry.uncompressed_size == 0xFFFFFFFFu) ok = ok && take(entry.uncompressed_size);
                if (entry.compressed_size == 0xFFFFFFFFu) ok = ok && take(entry.compressed_size);
                if (entry.local_header_offset == 0xFFFFFFFFu) ok = ok && take(entry.local_header_offset);
                if (disk_start == 0xFFFF) {
                    if (zl + 4 > len) {
                        ok = false;
                    } else {
                        disk_start = rd32(z + zl);
                    }
                }
                if (!ok) {
                    return std::unexpected(make_error(ZipErrorKind::Corrupt, "truncated ZIP64 extra field", entry.name));
                }
            }
            off += 4 + static_cast<std::size_t>(len);
        }
        pos += record;

        if (disk_start != 0) {
            return std::unexpected(make_error(ZipErrorKind::Unsupported, "entry on another disk", entry.name));
        }
        if ((entry.flags & 0x0001) != 0 || (entry.flags & 0x0040) != 0) {
            return std::unexpected(make_error(ZipErrorKind::Unsupported, "encrypted entries are not supported", entry.name));
        }
        const std::uint32_t unix_mode = entry.external_attributes >> 16;
        entry.is_symlink = entry.host_os == 3 && (unix_mode & 0170000u) == 0120000u;
        entry.is_directory = entry.name.ends_with('/') ||
                             (entry.host_os == 3 && (unix_mode & 0170000u) == 0040000u) ||
                             (entry.host_os != 3 && (entry.external_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && entry.uncompressed_size == 0);
        if (entry.is_symlink && limits.reject_symlinks) {
            return std::unexpected(make_error(ZipErrorKind::Symlink, "symbolic-link entries are not allowed", entry.name));
        }
        if (!entry.is_directory && entry.method != 0 && entry.method != 8) {
            return std::unexpected(make_error(
                ZipErrorKind::Unsupported, "compression method " + std::to_string(entry.method) + " is not supported", entry.name));
        }
        if (entry.method == 0 && entry.compressed_size != entry.uncompressed_size) {
            return std::unexpected(make_error(ZipErrorKind::Corrupt, "stored entry with different sizes", entry.name));
        }
        auto safe = sanitize_entry_name(entry.name, limits);
        if (!safe) {
            return std::unexpected(std::move(safe.error()));
        }
        entry.relative_path = std::move(*safe);
        if (entry.relative_path.empty() && !entry.is_directory) {
            return std::unexpected(make_error(ZipErrorKind::UnsafePath, "file entry without a name", entry.name));
        }

        if (entry.uncompressed_size > limits.max_entry_size) {
            return std::unexpected(make_error(ZipErrorKind::LimitExceeded, "entry larger than the per-entry limit", entry.name));
        }
        if (entry.uncompressed_size > limits.ratio_floor &&
            entry.uncompressed_size / std::max<std::uint64_t>(entry.compressed_size, 1) > limits.max_ratio) {
            return std::unexpected(make_error(ZipErrorKind::LimitExceeded, "compression ratio above the limit", entry.name));
        }
        total += entry.uncompressed_size;
        if (total > limits.max_total_size) {
            return std::unexpected(make_error(ZipErrorKind::LimitExceeded, "archive larger than the total size limit", entry.name));
        }
        if (entry.local_header_offset >= cd_offset || cd_offset - entry.local_header_offset < kLocalSize) {
            return std::unexpected(make_error(ZipErrorKind::Corrupt, "local header outside the data area", entry.name));
        }
        if (!entry.relative_path.empty() && !seen.insert(upper_key(entry.relative_path)).second) {
            return std::unexpected(make_error(ZipErrorKind::Duplicate, "duplicate entry path", entry.name));
        }
        entries.push_back(std::move(entry));
    }

    // Data ranges must not overlap: overlapping entries are how non-recursive zip bombs reuse one
    // compressed stream for thousands of files.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(entries.size());
    for (const auto& entry : entries) {
        auto start = data_offset(entry);
        if (!start) {
            return std::unexpected(std::move(start.error()));
        }
        ranges.emplace_back(entry.local_header_offset, *start + entry.compressed_size);
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].second) {
            return std::unexpected(make_error(ZipErrorKind::Corrupt, "overlapping entries (zip bomb)"));
        }
    }
    by_name.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        by_name.emplace(entries[i].name, i);
    }
    return {};
}

ZipResult<std::uint64_t> ZipReader::Impl::data_offset(const ZipEntry& entry) const {
    const std::uint64_t lh = entry.local_header_offset;
    if (lh > data.size() || data.size() - lh < kLocalSize || rd32(data.data() + lh) != kLocalSig) {
        return std::unexpected(make_error(ZipErrorKind::Corrupt, "bad local header", entry.name));
    }
    const std::byte* l = data.data() + lh;
    const std::uint64_t start = lh + kLocalSize + rd16(l + 26) + rd16(l + 28);
    if (start > central_offset || central_offset - start < entry.compressed_size) {
        return std::unexpected(make_error(ZipErrorKind::Corrupt, "entry data outside the data area", entry.name));
    }
    return start;
}

ZipReader::ZipReader(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
ZipReader::ZipReader(ZipReader&&) noexcept = default;
ZipReader& ZipReader::operator=(ZipReader&&) noexcept = default;
ZipReader::~ZipReader() = default;

ZipResult<ZipReader> ZipReader::open(const std::filesystem::path& path, ZipLimits limits) {
    auto impl = std::make_unique<Impl>();
    impl->limits = limits;
    impl->file = make_handle(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr));
    if (!impl->file) {
        const DWORD code = GetLastError();
        return std::unexpected(make_error(ZipErrorKind::Io, "cannot open archive: " + path.string(), {}, code));
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(impl->file.get(), &size)) {
        return std::unexpected(make_error(ZipErrorKind::Io, "cannot read archive size", {}, GetLastError()));
    }
    if (size.QuadPart < static_cast<LONGLONG>(kEocdSize)) {
        return std::unexpected(make_error(ZipErrorKind::NotZip, "file too small to be a ZIP archive"));
    }
    impl->mapping = make_handle(CreateFileMappingW(impl->file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!impl->mapping) {
        return std::unexpected(make_error(ZipErrorKind::Io, "CreateFileMapping failed", {}, GetLastError()));
    }
    const void* view = MapViewOfFile(impl->mapping.get(), FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        return std::unexpected(make_error(ZipErrorKind::Io, "MapViewOfFile failed", {}, GetLastError()));
    }
    impl->view.reset(view);
    impl->data = std::span<const std::byte>(static_cast<const std::byte*>(view), static_cast<std::size_t>(size.QuadPart));
    if (auto parsed = impl->parse(); !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    return ZipReader(std::move(impl));
}

ZipResult<ZipReader> ZipReader::open_memory(std::span<const std::byte> data, ZipLimits limits) {
    auto impl = std::make_unique<Impl>();
    impl->limits = limits;
    impl->data = data;
    if (auto parsed = impl->parse(); !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    return ZipReader(std::move(impl));
}

const std::vector<ZipEntry>& ZipReader::entries() const noexcept { return m_impl->entries; }
const ZipLimits& ZipReader::limits() const noexcept { return m_impl->limits; }
std::uint64_t ZipReader::total_uncompressed() const noexcept { return m_impl->total; }

const ZipEntry* ZipReader::find(std::string_view name) const noexcept {
    const auto it = m_impl->by_name.find(name);
    return it == m_impl->by_name.end() ? nullptr : &m_impl->entries[it->second];
}

ZipResult<void> ZipReader::read(
    const ZipEntry& entry, const std::function<bool(std::span<const std::byte>)>& sink, std::stop_token stop) const {
    if (entry.is_directory) {
        return {};
    }
    auto start = m_impl->data_offset(entry);
    if (!start) {
        return std::unexpected(std::move(start.error()));
    }
    const std::span<const std::byte> input = m_impl->data.subspan(static_cast<std::size_t>(*start), static_cast<std::size_t>(entry.compressed_size));
    std::uint32_t crc = 0;
    std::uint64_t produced = 0;
    const auto cancelled = [&]() { return make_error(ZipErrorKind::Cancelled, "cancelled", entry.name); };
    const auto aborted = [&]() { return make_error(ZipErrorKind::Cancelled, "aborted by receiver", entry.name); };

    if (entry.method == 0) {
        constexpr std::size_t kChunk = 1u << 20;
        for (std::size_t off = 0; off < input.size(); off += kChunk) {
            if (stop.stop_requested()) {
                return std::unexpected(cancelled());
            }
            const auto part = input.subspan(off, std::min(kChunk, input.size() - off));
            crc = crc32(part, crc);
            if (!sink(part)) {
                return std::unexpected(aborted());
            }
        }
        produced = input.size();
    } else {
        mz_stream stream{};
        if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
            return std::unexpected(make_error(ZipErrorKind::Io, "inflate init failed", entry.name));
        }
        struct InflateEnd {
            mz_stream* s;
            ~InflateEnd() { mz_inflateEnd(s); }
        } guard{&stream};
        std::vector<std::byte> out(256u * 1024u);
        std::size_t in_off = 0;
        bool finished = false;
        while (!finished) {
            if (stop.stop_requested()) {
                return std::unexpected(cancelled());
            }
            if (stream.avail_in == 0 && in_off < input.size()) {
                const std::size_t chunk = std::min<std::size_t>(input.size() - in_off, 1u << 30);
                stream.next_in = reinterpret_cast<const unsigned char*>(input.data() + in_off);
                stream.avail_in = static_cast<unsigned int>(chunk);
                in_off += chunk;
            }
            stream.next_out = reinterpret_cast<unsigned char*>(out.data());
            stream.avail_out = static_cast<unsigned int>(out.size());
            const int status = mz_inflate(&stream, MZ_NO_FLUSH);
            const std::size_t have = out.size() - stream.avail_out;
            if (status != MZ_OK && status != MZ_STREAM_END && !(status == MZ_BUF_ERROR && have > 0)) {
                return std::unexpected(make_error(ZipErrorKind::Corrupt, "invalid deflate data", entry.name));
            }
            if (have > 0) {
                produced += have;
                if (produced > entry.uncompressed_size) {
                    return std::unexpected(make_error(
                        ZipErrorKind::LimitExceeded, "entry inflates beyond its declared size (zip bomb)", entry.name));
                }
                const std::span<const std::byte> part(out.data(), have);
                crc = crc32(part, crc);
                if (!sink(part)) {
                    return std::unexpected(aborted());
                }
            }
            if (status == MZ_STREAM_END) {
                finished = true;
            } else if (have == 0 && stream.avail_in == 0 && in_off >= input.size()) {
                return std::unexpected(make_error(ZipErrorKind::Corrupt, "truncated deflate stream", entry.name));
            }
        }
    }
    if (produced != entry.uncompressed_size) {
        return std::unexpected(make_error(ZipErrorKind::CrcMismatch, "entry size does not match its header", entry.name));
    }
    if (crc != entry.crc32) {
        return std::unexpected(make_error(ZipErrorKind::CrcMismatch, "CRC-32 mismatch", entry.name));
    }
    return {};
}

ZipResult<std::vector<std::byte>> ZipReader::read_all(const ZipEntry& entry) const {
    std::vector<std::byte> out;
    out.reserve(static_cast<std::size_t>(entry.uncompressed_size));
    auto r = read(entry, [&out](std::span<const std::byte> part) {
        out.insert(out.end(), part.begin(), part.end());
        return true;
    });
    if (!r) {
        return std::unexpected(std::move(r.error()));
    }
    return out;
}

ZipResult<ExtractReport> ZipReader::extract_to(const std::filesystem::path& destination, const ExtractOptions& options) const {
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::absolute(destination, ec);
    if (ec) {
        return std::unexpected(make_error(ZipErrorKind::Io, "bad destination: " + ec.message()));
    }
    std::filesystem::create_directories(root, ec);
    if (ec) {
        return std::unexpected(make_error(ZipErrorKind::Io, "cannot create destination: " + ec.message()));
    }
    if (!options.overwrite && !std::filesystem::is_empty(root, ec)) {
        return std::unexpected(make_error(ZipErrorKind::Io, "destination is not empty"));
    }
    const UniqueHandle root_handle = make_handle(CreateFileW(
        extended(root).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!root_handle) {
        return std::unexpected(make_error(ZipErrorKind::Io, "cannot open destination", {}, GetLastError()));
    }
    const auto root_final = final_path(root_handle.get());
    if (!root_final) {
        return std::unexpected(make_error(ZipErrorKind::Io, "cannot resolve destination", {}, GetLastError()));
    }

    ExtractReport report;
    ExtractProgress progress{.entries_done = 0, .entries_total = m_impl->entries.size(), .bytes_written = 0, .bytes_total = m_impl->total};
    std::unordered_set<std::wstring> top_seen;
    const auto note_top = [&](const ZipEntry& entry) {
        if (entry.relative_path.empty()) {
            return;
        }
        const std::filesystem::path first = *entry.relative_path.begin();
        if (top_seen.insert(upper_key(first)).second) {
            report.top_level.push_back(core::wide_to_utf8(first.native()));
        }
    };

    for (const auto& entry : m_impl->entries) {
        if (options.stop.stop_requested()) {
            return std::unexpected(make_error(ZipErrorKind::Cancelled, "cancelled"));
        }
        if (entry.relative_path.empty()) {
            continue;
        }
        note_top(entry);
        const std::filesystem::path target = root / entry.relative_path;
        if (entry.is_directory) {
            std::filesystem::create_directories(extended(target), ec);
            if (ec) {
                return std::unexpected(make_error(ZipErrorKind::Io, "cannot create directory: " + ec.message(), entry.name));
            }
            ++report.directories;
        } else {
            std::filesystem::create_directories(extended(target.parent_path()), ec);
            if (ec) {
                return std::unexpected(make_error(ZipErrorKind::Io, "cannot create directory: " + ec.message(), entry.name));
            }
            const std::wstring target_ext = extended(target);
            UniqueHandle out = make_handle(CreateFileW(
                target_ext.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, options.overwrite ? CREATE_ALWAYS : CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (!out) {
                const DWORD code = GetLastError();
                return std::unexpected(make_error(ZipErrorKind::Io, "cannot create file", entry.name, code));
            }
            const auto discard = [&]() {
                FILE_DISPOSITION_INFO dispose{TRUE};
                SetFileInformationByHandle(out.get(), FileDispositionInfo, &dispose, sizeof(dispose));
                out.reset();
            };
            const auto where = final_path(out.get());
            if (!where || !path_within(*where, *root_final)) {
                discard();
                return std::unexpected(make_error(ZipErrorKind::UnsafePath, "entry resolves outside the destination", entry.name));
            }
            if (entry.uncompressed_size > 0) {
                FILE_ALLOCATION_INFO alloc{};
                alloc.AllocationSize.QuadPart = static_cast<LONGLONG>(entry.uncompressed_size);
                SetFileInformationByHandle(out.get(), FileAllocationInfo, &alloc, sizeof(alloc));
            }
            DWORD write_error = 0;
            std::uint64_t since_report = 0;
            auto r = read(entry, [&](std::span<const std::byte> part) {
                std::size_t off = 0;
                while (off < part.size()) {
                    DWORD written = 0;
                    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(part.size() - off, 1u << 30));
                    if (!WriteFile(out.get(), part.data() + off, want, &written, nullptr) || written == 0) {
                        write_error = GetLastError();
                        return false;
                    }
                    off += written;
                }
                progress.bytes_written += part.size();
                since_report += part.size();
                if (options.on_progress && since_report >= (4u << 20)) {
                    since_report = 0;
                    options.on_progress(progress);
                }
                return true;
            }, options.stop);
            if (!r) {
                discard();
                if (write_error != 0) {
                    return std::unexpected(make_error(ZipErrorKind::Io, "write failed", entry.name, write_error));
                }
                return std::unexpected(std::move(r.error()));
            }
            const FILETIME ft = dos_to_filetime(entry.dos_date, entry.dos_time);
            if (ft.dwHighDateTime != 0 || ft.dwLowDateTime != 0) {
                SetFileTime(out.get(), nullptr, nullptr, &ft);
            }
            ++report.files;
            report.bytes += entry.uncompressed_size;
        }
        ++progress.entries_done;
        if (options.on_progress) {
            options.on_progress(progress);
        }
    }
    return report;
}

std::vector<std::byte> deflate_raw(std::span<const std::byte> data, int level) {
    mz_stream s{};
    mz_deflateInit2(&s, level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY);
    std::vector<std::byte> out(static_cast<std::size_t>(mz_deflateBound(&s, static_cast<mz_ulong>(data.size()))) + 64);
    s.next_in = reinterpret_cast<const unsigned char*>(data.data());
    s.avail_in = static_cast<unsigned int>(data.size());
    s.next_out = reinterpret_cast<unsigned char*>(out.data());
    s.avail_out = static_cast<unsigned int>(out.size());
    mz_deflate(&s, MZ_FINISH);
    out.resize(out.size() - s.avail_out);
    mz_deflateEnd(&s);
    return out;
}

std::vector<std::byte> gzip_compress(std::span<const std::byte> data, int level) {
    std::vector<std::byte> out;
    static constexpr unsigned char kHeader[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0x0b};
    for (const unsigned char b : kHeader) {
        out.push_back(static_cast<std::byte>(b));
    }
    const auto body = deflate_raw(data, level);
    out.insert(out.end(), body.begin(), body.end());
    wr32(out, crc32(data));
    wr32(out, static_cast<std::uint32_t>(data.size() & 0xFFFFFFFFu));
    return out;
}

ZipResult<std::vector<std::byte>> inflate_raw(std::span<const std::byte> data, std::size_t max_output) {
    mz_stream s{};
    if (mz_inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
        return std::unexpected(make_error(ZipErrorKind::Io, "inflate init failed"));
    }
    std::vector<std::byte> out;
    std::vector<std::byte> chunk(64u * 1024u);
    s.next_in = reinterpret_cast<const unsigned char*>(data.data());
    s.avail_in = static_cast<unsigned int>(data.size());
    int status = MZ_OK;
    while (status != MZ_STREAM_END) {
        s.next_out = reinterpret_cast<unsigned char*>(chunk.data());
        s.avail_out = static_cast<unsigned int>(chunk.size());
        status = mz_inflate(&s, MZ_NO_FLUSH);
        const std::size_t have = chunk.size() - s.avail_out;
        if (status != MZ_OK && status != MZ_STREAM_END && !(status == MZ_BUF_ERROR && have > 0)) {
            mz_inflateEnd(&s);
            return std::unexpected(make_error(ZipErrorKind::Corrupt, "invalid deflate data"));
        }
        if (out.size() + have > max_output) {
            mz_inflateEnd(&s);
            return std::unexpected(make_error(ZipErrorKind::LimitExceeded, "inflated data exceeds the limit"));
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(have));
        if (have == 0 && s.avail_in == 0 && status != MZ_STREAM_END) {
            mz_inflateEnd(&s);
            return std::unexpected(make_error(ZipErrorKind::Corrupt, "truncated deflate stream"));
        }
    }
    mz_inflateEnd(&s);
    return out;
}

// ---- ZipWriter --------------------------------------------------------------------------------------------

ZipWriter::ZipWriter() : ZipWriter(Options{}) {}
ZipWriter::ZipWriter(Options options) : m_options(options) {}

void ZipWriter::add(std::string_view name, std::span<const std::byte> data, std::uint32_t unix_mode) {
    add_raw(name, data, unix_mode, data.size(), false);
}

void ZipWriter::add(std::string_view name, std::string_view text, std::uint32_t unix_mode) {
    add(name, std::as_bytes(std::span<const char>(text.data(), text.size())), unix_mode);
}

void ZipWriter::add_directory(std::string_view name) {
    std::string n(name);
    if (!n.ends_with('/')) {
        n.push_back('/');
    }
    add_raw(n, {}, 0, 0, true);
}

void ZipWriter::add_with_declared_size(std::string_view name, std::span<const std::byte> data, std::uint64_t declared_size) {
    add_raw(name, data, 0, declared_size, false);
}

void ZipWriter::add_raw(std::string_view name, std::span<const std::byte> data, std::uint32_t unix_mode,
                        std::uint64_t declared_size, bool directory) {
    Record rec;
    rec.name = std::string(name);
    rec.offset = m_out.size();
    rec.uncompressed = declared_size;
    rec.crc = crc32(data);
    std::vector<std::byte> payload;
    const bool deflate = m_options.deflate && !directory && !data.empty();
    if (deflate) {
        mz_stream s{};
        mz_deflateInit2(&s, m_options.level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY);
        payload.resize(static_cast<std::size_t>(mz_deflateBound(&s, static_cast<mz_ulong>(data.size()))) + 64);
        s.next_in = reinterpret_cast<const unsigned char*>(data.data());
        s.avail_in = static_cast<unsigned int>(data.size());
        s.next_out = reinterpret_cast<unsigned char*>(payload.data());
        s.avail_out = static_cast<unsigned int>(payload.size());
        mz_deflate(&s, MZ_FINISH);
        payload.resize(payload.size() - s.avail_out);
        mz_deflateEnd(&s);
        rec.method = 8;
    } else {
        payload.assign(data.begin(), data.end());
        rec.method = 0;
    }
    rec.compressed = payload.size();
    if (unix_mode != 0) {
        rec.made_by = static_cast<std::uint16_t>((3u << 8) | 20u);
        rec.external = unix_mode << 16;
    } else if (directory) {
        rec.external = FILE_ATTRIBUTE_DIRECTORY;
    }
    const bool z64 = m_options.force_zip64;
    wr32(m_out, kLocalSig);
    wr16(m_out, z64 ? 45 : 20);
    wr16(m_out, 0x0800);
    wr16(m_out, rec.method);
    wr16(m_out, 0x6000); // 12:00
    wr16(m_out, 0x5A21); // 2025-01-01
    wr32(m_out, rec.crc);
    wr32(m_out, z64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(rec.compressed));
    wr32(m_out, z64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(rec.uncompressed));
    wr16(m_out, static_cast<std::uint32_t>(rec.name.size()));
    wr16(m_out, z64 ? 20 : 0);
    wr_bytes(m_out, rec.name);
    if (z64) {
        wr16(m_out, 0x0001);
        wr16(m_out, 16);
        wr64(m_out, rec.uncompressed);
        wr64(m_out, rec.compressed);
    }
    m_out.insert(m_out.end(), payload.begin(), payload.end());
    m_records.push_back(std::move(rec));
}

std::vector<std::byte> ZipWriter::finish() {
    const bool z64 = m_options.force_zip64;
    const std::uint64_t cd_offset = m_out.size();
    for (const auto& rec : m_records) {
        wr32(m_out, kCentralSig);
        wr16(m_out, rec.made_by);
        wr16(m_out, z64 ? 45 : 20);
        wr16(m_out, 0x0800);
        wr16(m_out, rec.method);
        wr16(m_out, 0x6000);
        wr16(m_out, 0x5A21);
        wr32(m_out, rec.crc);
        wr32(m_out, z64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(rec.compressed));
        wr32(m_out, z64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(rec.uncompressed));
        wr16(m_out, static_cast<std::uint32_t>(rec.name.size()));
        wr16(m_out, z64 ? 28 : 0);
        wr16(m_out, 0);
        wr16(m_out, 0);
        wr16(m_out, 0);
        wr32(m_out, rec.external);
        wr32(m_out, z64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(rec.offset));
        wr_bytes(m_out, rec.name);
        if (z64) {
            wr16(m_out, 0x0001);
            wr16(m_out, 24);
            wr64(m_out, rec.uncompressed);
            wr64(m_out, rec.compressed);
            wr64(m_out, rec.offset);
        }
    }
    const std::uint64_t cd_size = m_out.size() - cd_offset;
    const std::uint64_t count = m_records.size();
    if (z64) {
        const std::uint64_t eocd64 = m_out.size();
        wr32(m_out, kEocd64Sig);
        wr64(m_out, kEocd64Size - 12);
        wr16(m_out, 45);
        wr16(m_out, 45);
        wr32(m_out, 0);
        wr32(m_out, 0);
        wr64(m_out, count);
        wr64(m_out, count);
        wr64(m_out, cd_size);
        wr64(m_out, cd_offset);
        wr32(m_out, kEocd64LocatorSig);
        wr32(m_out, 0);
        wr64(m_out, eocd64);
        wr32(m_out, 1);
    }
    wr32(m_out, kEocdSig);
    wr16(m_out, 0);
    wr16(m_out, 0);
    wr16(m_out, z64 ? 0xFFFF : static_cast<std::uint32_t>(count));
    wr16(m_out, z64 ? 0xFFFF : static_cast<std::uint32_t>(count));
    wr32(m_out, z64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(cd_size));
    wr32(m_out, z64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(cd_offset));
    wr16(m_out, 0);
    m_records.clear();
    return std::move(m_out);
}

} // namespace boson::archive
