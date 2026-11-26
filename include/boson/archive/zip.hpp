#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace boson::archive {

enum class ZipErrorKind : std::uint8_t {
    Io,            ///< the archive or a destination file could not be read / written
    NotZip,        ///< no end-of-central-directory record
    Corrupt,       ///< inconsistent headers, offsets outside the file, truncated data
    Unsupported,   ///< encrypted entries, compression methods other than stored / deflate, multi-disk
    UnsafePath,    ///< traversal ("..", absolute, drive prefix, UNC, device name, ADS ":" ...)
    Symlink,       ///< a symbolic-link entry
    Duplicate,     ///< two entries map to the same (case-insensitive) path
    LimitExceeded, ///< entry count / size / ratio limits (zip bombs)
    CrcMismatch,   ///< data does not match the CRC-32 or declared size of its entry
    Cancelled,
};

struct ZipError {
    ZipErrorKind kind = ZipErrorKind::Io;
    std::string message;
    std::string entry;              ///< entry name when the error concerns one
    std::uint32_t win32_error = 0;
};

template <typename T>
using ZipResult = std::expected<T, ZipError>;

[[nodiscard]] std::string_view to_string(ZipErrorKind kind) noexcept;

/// @brief Resource limits checked before (declared sizes) and while (actual output) inflating.
struct ZipLimits {
    std::uint64_t max_entries = 65'536;
    std::uint64_t max_entry_size = 1ull << 30;       ///< uncompressed bytes per entry (1 GiB)
    std::uint64_t max_total_size = 4ull << 30;       ///< uncompressed bytes over all entries (4 GiB)
    /// Max uncompressed / compressed ratio per entry. Only applied once an entry exceeds ratio_floor bytes,
    /// so tiny highly-compressible files (empty lines, zero-filled headers) are fine.
    std::uint64_t max_ratio = 200;
    std::uint64_t ratio_floor = 1ull << 20;
    std::size_t max_name_bytes = 1024;
    std::size_t max_depth = 64;
    /// Reject symbolic-link entries (Unix mode S_IFLNK). true is the only safe choice for extraction.
    bool reject_symlinks = true;
};

struct ZipEntry {
    std::string name;            ///< as stored, '\\' converted to '/'
    std::filesystem::path relative_path; ///< validated, safe relative path (empty for the root)
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t local_header_offset = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t external_attributes = 0;
    std::uint16_t method = 0;    ///< 0 stored, 8 deflate
    std::uint16_t flags = 0;
    std::uint16_t dos_time = 0;
    std::uint16_t dos_date = 0;
    std::uint8_t host_os = 0;    ///< upper byte of "version made by" (0 DOS/Windows, 3 Unix)
    bool is_directory = false;
    bool is_symlink = false;
    bool zip64 = false;
};

/// @brief Validates an archive entry name and turns it into a safe relative path. Rejects empty names,
/// absolute paths ("/x", "\\x"), drive prefixes ("C:"), UNC / device paths, ".." components, ':' anywhere
/// (alternate data streams), control characters, reserved device names (CON, NUL, COM1, LPT1, ...), and
/// components ending in '.' or ' ' (Windows strips them, which would alias another entry).
[[nodiscard]] ZipResult<std::filesystem::path> sanitize_entry_name(std::string_view name, const ZipLimits& limits = {});

struct ExtractProgress {
    std::uint64_t entries_done = 0;
    std::uint64_t entries_total = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t bytes_total = 0; ///< sum of declared uncompressed sizes
};

struct ExtractOptions {
    std::stop_token stop;
    /// Called on the extracting thread after each entry (and at most every ~4 MiB inside big entries).
    std::function<void(const ExtractProgress&)> on_progress;
    /// false: the destination directory must be empty or absent (staging use). true: existing files are
    /// replaced.
    bool overwrite = false;
};

struct ExtractReport {
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    std::uint64_t bytes = 0;
    /// Distinct first path components (e.g. the add-on folders an archive installs).
    std::vector<std::string> top_level;
};

/// @brief Read-only ZIP archive (stored + deflate, ZIP64). The file is memory-mapped (no copy); every entry
/// name is validated and the limits are checked when it is opened, before anything is extracted. CRC-32
/// and sizes are verified while inflating. Blocking: use it from a pool thread. Thread-compatible: reading
/// different entries from several threads at once is safe (the mapping is read-only).
class ZipReader {
public:
    [[nodiscard]] static ZipResult<ZipReader> open(const std::filesystem::path& path, ZipLimits limits = {});
    /// @brief Archive in memory. The buffer must outlive the reader.
    [[nodiscard]] static ZipResult<ZipReader> open_memory(std::span<const std::byte> data, ZipLimits limits = {});

    ZipReader(ZipReader&&) noexcept;
    ZipReader& operator=(ZipReader&&) noexcept;
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;
    ~ZipReader();

    [[nodiscard]] const std::vector<ZipEntry>& entries() const noexcept;
    [[nodiscard]] const ZipLimits& limits() const noexcept;
    [[nodiscard]] std::uint64_t total_uncompressed() const noexcept;
    /// @brief Finds an entry by its stored name ('/' separators, case-sensitive).
    [[nodiscard]] const ZipEntry* find(std::string_view name) const noexcept;

    /// @brief Streams an entry's uncompressed bytes to sink (return false to stop). Verifies size and CRC.
    [[nodiscard]] ZipResult<void> read(
        const ZipEntry& entry,
        const std::function<bool(std::span<const std::byte>)>& sink,
        std::stop_token stop = {}) const;
    /// @brief Whole entry into memory (subject to max_entry_size).
    [[nodiscard]] ZipResult<std::vector<std::byte>> read_all(const ZipEntry& entry) const;

    /// @brief Extracts every entry below destination (created if absent). Files are created with
    /// CREATE_NEW unless overwrite is set, and each opened handle is checked to resolve inside destination
    /// (no junction / reparse-point escape). On failure the partially written file is deleted; already
    /// extracted entries stay (extract into a staging directory and discard it, see fs::InstallTransaction).
    [[nodiscard]] ZipResult<ExtractReport> extract_to(
        const std::filesystem::path& destination, const ExtractOptions& options = {}) const;

private:
    struct Impl;
    explicit ZipReader(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

/// @brief CRC-32 (IEEE, as used by ZIP / gzip). Pass the previous value to continue.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t previous = 0) noexcept;

/// @brief Raw deflate (RFC 1951) of data, level 1..10.
[[nodiscard]] std::vector<std::byte> deflate_raw(std::span<const std::byte> data, int level = 6);
/// @brief gzip member (RFC 1952) of data: header + raw deflate + CRC-32 + size.
[[nodiscard]] std::vector<std::byte> gzip_compress(std::span<const std::byte> data, int level = 6);
/// @brief Inflates raw deflate data; fails when the output would exceed max_output bytes.
[[nodiscard]] ZipResult<std::vector<std::byte>> inflate_raw(std::span<const std::byte> data, std::size_t max_output);

/// @brief Minimal ZIP writer (stored or deflate, optional ZIP64 records). Used by tests and by apps that
/// export small archives (logs, backups). Entry names are written as given - including unsafe ones - so
/// tests can build hostile archives; validate names yourself when they come from user input.
class ZipWriter {
public:
    struct Options {
        bool deflate = true;
        int level = 6;          ///< 1..10 (miniz levels)
        bool force_zip64 = false; ///< write ZIP64 extra fields + ZIP64 end records even for small archives
    };

    ZipWriter();
    explicit ZipWriter(Options options);

    /// @brief Adds a file entry. unix_mode (e.g. 0120777 for a symlink) sets the Unix host + mode bits.
    void add(std::string_view name, std::span<const std::byte> data, std::uint32_t unix_mode = 0);
    void add(std::string_view name, std::string_view text, std::uint32_t unix_mode = 0);
    void add_directory(std::string_view name);
    /// @brief Adds an entry whose central-directory sizes lie (declared_size): for zip-bomb tests.
    void add_with_declared_size(std::string_view name, std::span<const std::byte> data, std::uint64_t declared_size);
    [[nodiscard]] std::vector<std::byte> finish();

private:
    struct Record {
        std::string name;
        std::uint64_t offset = 0;
        std::uint64_t compressed = 0;
        std::uint64_t uncompressed = 0;
        std::uint32_t crc = 0;
        std::uint16_t method = 0;
        std::uint32_t external = 0;
        std::uint16_t made_by = 20;
    };
    void add_raw(std::string_view name, std::span<const std::byte> data, std::uint32_t unix_mode,
                 std::uint64_t declared_size, bool directory);

    Options m_options;
    std::vector<std::byte> m_out;
    std::vector<Record> m_records;
};

} // namespace boson::archive
