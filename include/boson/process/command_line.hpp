#pragma once

#include <windows.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::process {

/// @brief Failure of a process / pipe operation. code is a Win32 error code; message is UTF-8.
struct ProcessError {
    std::uint32_t code = 0;
    std::string message;
};

template <typename T>
using ProcessResult = std::expected<T, ProcessError>;

/// @brief ProcessError for code with "context: <FormatMessage text>" as message.
[[nodiscard]] ProcessError make_error(std::uint32_t code, std::string_view context);
/// @brief make_error(GetLastError(), context).
[[nodiscard]] ProcessError last_error(std::string_view context);

// ---- Command lines ---------------------------------------------------------------------------------------
//
// Windows passes a single command-line string to the child; every program splits it again itself. Almost
// all of them (the MSVC / UCRT startup code, CommandLineToArgvW, Go, Rust, Node, Python) use the same rules:
// whitespace separates arguments, "..." groups, 2n backslashes before a quote become n backslashes and the
// quote toggles quoting, 2n+1 backslashes before a quote become n backslashes and a literal quote, and
// backslashes not followed by a quote are literal. argv[0] is different: it ends at the next quote (or
// whitespace when unquoted) and has no escapes, so a program path may not contain '"'.
//
// cmd.exe (and therefore every .bat / .cmd file, which CreateProcess silently runs through cmd.exe /c) has
// its own, incompatible parser with %VAR% expansion and & | < > ^ ( ) metacharacters. build_batch_command_line
// escapes arguments for it the way Rust's std::process does after CVE-2024-24576 ("BatBadBut").

/// @brief Quotes one argument (argv[1..]) so CommandLineToArgvW / the CRT recover it exactly.
[[nodiscard]] std::wstring quote_argument(std::wstring_view arg);
/// @brief Quotes argv[0]. Fails with ERROR_BAD_ARGUMENTS when program contains '"' or a NUL.
[[nodiscard]] ProcessResult<std::wstring> quote_program_name(std::wstring_view program);
/// @brief Quoted program followed by quoted arguments. Fails on NUL characters or a '"' in program.
[[nodiscard]] ProcessResult<std::wstring> build_command_line(std::wstring_view program, std::span<const std::wstring> args);

/// @brief One argument escaped for a batch file run through cmd.exe: every ASCII character other than
/// letters, digits and #$*+-./:?@\_ forces quoting, '"' is doubled, '%' becomes %%cd:~,% (which cmd expands
/// to a literal '%'), and a trailing backslash forces quoting. Fails (ERROR_BAD_ARGUMENTS) on NUL, CR or LF:
/// cmd.exe truncates the command line there and nothing can escape them.
[[nodiscard]] ProcessResult<std::wstring> quote_cmd_argument(std::wstring_view arg);
/// @brief The full command line for running script (a .bat / .cmd path) with args:
///   cmd.exe /e:ON /v:OFF /d /c ""<script>" <quote_cmd_argument(arg)>..."
/// Pass system_cmd_path() as the application name so cmd.exe cannot be hijacked through the search path.
[[nodiscard]] ProcessResult<std::wstring> build_batch_command_line(std::wstring_view script, std::span<const std::wstring> args);
/// @brief true for a path ending in .bat or .cmd (case-insensitive, trailing dots and spaces ignored).
[[nodiscard]] bool is_batch_file(std::wstring_view path) noexcept;
/// @brief "<GetSystemDirectoryW>\cmd.exe".
[[nodiscard]] std::wstring system_cmd_path();
/// @brief CommandLineToArgvW (an empty command line yields an empty vector).
[[nodiscard]] std::vector<std::wstring> split_command_line(std::wstring_view command_line);

// ---- Environment -----------------------------------------------------------------------------------------

struct EnvVar {
    std::wstring name;
    std::wstring value;
};

/// @brief GetEnvironmentStringsW parsed into name/value pairs, including the hidden "=C:" entries.
[[nodiscard]] std::vector<EnvVar> current_environment();
/// @brief Case-insensitive lookup (Windows environment names are case-insensitive).
[[nodiscard]] std::optional<std::wstring> find_env(std::span<const EnvVar> env, std::wstring_view name);
/// @brief Inserts or replaces name (case-insensitive); an empty optional removes it.
void set_env(std::vector<EnvVar>& env, std::wstring_view name, std::optional<std::wstring_view> value);
/// @brief CREATE_UNICODE_ENVIRONMENT block: entries sorted case-insensitively ("=X:" first), each
/// "name=value\0", terminated by an extra "\0". Fails on a name that is empty, contains '=' after the first
/// character, or has a NUL anywhere.
[[nodiscard]] ProcessResult<std::vector<wchar_t>> build_environment_block(std::span<const EnvVar> env);

// ---- Executable lookup -----------------------------------------------------------------------------------

struct ResolveOptions {
    /// PATH to search ("" = this process's PATH). The current directory is never searched.
    std::wstring path;
    /// Base for relative paths ("" = this process's current directory).
    std::wstring cwd;
    /// Also accept .bat / .cmd (they run through cmd.exe).
    bool allow_batch_files = true;
};

/// @brief Absolute path of the program name would start. A name with a directory part ("bin\\tool",
/// "C:\\x\\tool.exe", "./tool") is taken relative to cwd; a bare name is searched in PATH, then the system and
/// Windows directories. Without an extension, .com and .exe (then .bat and .cmd when allowed) are tried.
/// ERROR_FILE_NOT_FOUND when nothing matches.
[[nodiscard]] ProcessResult<std::wstring> resolve_executable(std::wstring_view name, const ResolveOptions& options = {});

} // namespace boson::process
