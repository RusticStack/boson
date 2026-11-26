#include <boson/process/command_line.hpp>

#include <boson/app/lifecycle.hpp>
#include <boson/core/result.hpp>

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace boson::process {

namespace {

struct LocalFreeDeleter {
    void operator()(LPWSTR* p) const noexcept { LocalFree(static_cast<HLOCAL>(p)); }
};

struct EnvStringsDeleter {
    void operator()(wchar_t* p) const noexcept { FreeEnvironmentStringsW(p); }
};

[[nodiscard]] bool iequals(std::wstring_view a, std::wstring_view b) noexcept {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] int icompare(std::wstring_view a, std::wstring_view b) noexcept {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) - CSTR_EQUAL;
}

[[nodiscard]] bool is_file(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

[[nodiscard]] std::wstring_view trim_trailing_dots_spaces(std::wstring_view path) noexcept {
    while (!path.empty() && (path.back() == L'.' || path.back() == L' ')) {
        path.remove_suffix(1);
    }
    return path;
}

[[nodiscard]] bool has_extension(std::wstring_view name) noexcept {
    const std::size_t slash = name.find_last_of(L"\\/");
    const std::size_t dot = name.find_last_of(L'.');
    return dot != std::wstring_view::npos && (slash == std::wstring_view::npos || dot > slash) && dot + 1 < name.size();
}

[[nodiscard]] std::wstring directory_of(UINT (*getter)(LPWSTR, UINT)) {
    std::array<wchar_t, MAX_PATH + 1> buffer{};
    const UINT n = getter(buffer.data(), static_cast<UINT>(buffer.size()));
    if (n == 0 || n >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), n);
}

UINT get_windows_dir(LPWSTR buffer, UINT size) {
    return GetWindowsDirectoryW(buffer, size);
}

UINT get_system_dir(LPWSTR buffer, UINT size) {
    return GetSystemDirectoryW(buffer, size);
}

[[nodiscard]] std::wstring env_value(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    value.resize(written);
    return value;
}

} // namespace

ProcessError make_error(std::uint32_t code, std::string_view context) {
    ProcessError error{.code = code, .message = std::string(context)};
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    error.message += ": ";
    if (length != 0 && text != nullptr) {
        std::wstring_view view(text, length);
        while (!view.empty() && (view.back() == L'\r' || view.back() == L'\n' || view.back() == L' ' || view.back() == L'.')) {
            view.remove_suffix(1);
        }
        error.message += core::wide_to_utf8(view);
        LocalFree(text);
    } else {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "error %lu", static_cast<unsigned long>(code));
        error.message += buffer;
    }
    return error;
}

ProcessError last_error(std::string_view context) {
    return make_error(GetLastError(), context);
}

std::wstring quote_argument(std::wstring_view arg) {
    return app::quote_argument(arg);
}

ProcessResult<std::wstring> quote_program_name(std::wstring_view program) {
    if (program.empty() || program.find_first_of(std::wstring_view(L"\"\0", 2)) != std::wstring_view::npos) {
        return std::unexpected(ProcessError{.code = ERROR_BAD_ARGUMENTS,
                                            .message = "program path is empty or contains a quote / NUL character"});
    }
    std::wstring out;
    out.reserve(program.size() + 2);
    out.push_back(L'"');
    out += program;
    out.push_back(L'"');
    return out;
}

ProcessResult<std::wstring> build_command_line(std::wstring_view program, std::span<const std::wstring> args) {
    auto out = quote_program_name(program);
    if (!out) {
        return out;
    }
    for (const auto& arg : args) {
        if (arg.find(L'\0') != std::wstring::npos) {
            return std::unexpected(ProcessError{.code = ERROR_BAD_ARGUMENTS, .message = "argument contains a NUL character"});
        }
        out->push_back(L' ');
        *out += quote_argument(arg);
    }
    if (out->size() >= 32767) {
        return std::unexpected(ProcessError{.code = ERROR_FILENAME_EXCED_RANGE, .message = "command line longer than 32767 characters"});
    }
    return out;
}

ProcessResult<std::wstring> quote_cmd_argument(std::wstring_view arg) {
    if (arg.find_first_of(std::wstring_view(L"\0\r\n", 3)) != std::wstring_view::npos) {
        return std::unexpected(ProcessError{.code = ERROR_BAD_ARGUMENTS,
                                            .message = "batch file arguments cannot contain NUL, CR or LF characters"});
    }
    constexpr std::wstring_view kUnquoted = L"#$*+-./:?@\\_";
    bool quote = arg.empty() || arg.back() == L'\\';
    for (const wchar_t c : arg) {
        const bool ascii = c < 0x80;
        const bool alnum = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
        const bool control = c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0);
        if ((ascii && !alnum && kUnquoted.find(c) == std::wstring_view::npos) || control) {
            quote = true;
        }
    }
    std::wstring out;
    out.reserve(arg.size() + 8);
    if (quote) {
        out.push_back(L'"');
    }
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else {
            if (c == L'"') {
                out.append(backslashes, L'\\'); // 2n backslashes before the doubled quote
                out.push_back(L'"');
            } else if (c == L'%') {
                out += L"%%cd:~,"; // "%%cd:~,%" expands to a single literal '%'
            }
            backslashes = 0;
        }
        out.push_back(c);
    }
    if (quote) {
        out.append(backslashes, L'\\');
        out.push_back(L'"');
    }
    return out;
}

ProcessResult<std::wstring> build_batch_command_line(std::wstring_view script, std::span<const std::wstring> args) {
    if (script.empty() || script.find_first_of(std::wstring_view(L"\"\0", 2)) != std::wstring_view::npos || script.back() == L'\\') {
        return std::unexpected(ProcessError{.code = ERROR_BAD_ARGUMENTS,
                                            .message = "batch file path is empty, contains a quote or ends with a backslash"});
    }
    std::wstring out = L"cmd.exe /e:ON /v:OFF /d /c \"\"";
    out += script;
    out.push_back(L'"');
    for (const auto& arg : args) {
        auto quoted = quote_cmd_argument(arg);
        if (!quoted) {
            return std::unexpected(std::move(quoted.error()));
        }
        out.push_back(L' ');
        out += *quoted;
    }
    out.push_back(L'"');
    if (out.size() >= 8191) {
        return std::unexpected(ProcessError{.code = ERROR_FILENAME_EXCED_RANGE, .message = "cmd.exe command line longer than 8191 characters"});
    }
    return out;
}

bool is_batch_file(std::wstring_view path) noexcept {
    path = trim_trailing_dots_spaces(path);
    if (path.size() < 4) {
        return false;
    }
    const std::wstring_view ext = path.substr(path.size() - 4);
    return iequals(ext, L".bat") || iequals(ext, L".cmd");
}

std::wstring system_cmd_path() {
    std::wstring dir = directory_of(&get_system_dir);
    if (dir.empty()) {
        dir = env_value(L"SystemRoot") + L"\\System32";
    }
    if (!dir.empty() && dir.back() != L'\\') {
        dir.push_back(L'\\');
    }
    return dir + L"cmd.exe";
}

std::vector<std::wstring> split_command_line(std::wstring_view command_line) {
    std::vector<std::wstring> out;
    if (command_line.empty()) {
        return out;
    }
    const std::wstring copy(command_line);
    int argc = 0;
    const std::unique_ptr<LPWSTR, LocalFreeDeleter> argv(CommandLineToArgvW(copy.c_str(), &argc));
    if (!argv) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        out.emplace_back(argv.get()[i]);
    }
    return out;
}

std::vector<EnvVar> current_environment() {
    std::vector<EnvVar> out;
    const std::unique_ptr<wchar_t, EnvStringsDeleter> block(GetEnvironmentStringsW());
    if (!block) {
        return out;
    }
    const wchar_t* p = block.get();
    while (*p != L'\0') {
        const std::wstring_view entry(p);
        // "=C:=C:\dir" entries: the name starts with '=', so look for the separator after the first character.
        const std::size_t eq = entry.find(L'=', 1);
        if (eq != std::wstring_view::npos) {
            out.push_back(EnvVar{.name = std::wstring(entry.substr(0, eq)), .value = std::wstring(entry.substr(eq + 1))});
        }
        p += entry.size() + 1;
    }
    return out;
}

std::optional<std::wstring> find_env(std::span<const EnvVar> env, std::wstring_view name) {
    for (const auto& var : env) {
        if (iequals(var.name, name)) {
            return var.value;
        }
    }
    return std::nullopt;
}

void set_env(std::vector<EnvVar>& env, std::wstring_view name, std::optional<std::wstring_view> value) {
    const auto it = std::find_if(env.begin(), env.end(), [&](const EnvVar& v) { return iequals(v.name, name); });
    if (!value) {
        if (it != env.end()) {
            env.erase(it);
        }
        return;
    }
    if (it != env.end()) {
        it->value.assign(*value);
    } else {
        env.push_back(EnvVar{.name = std::wstring(name), .value = std::wstring(*value)});
    }
}

ProcessResult<std::vector<wchar_t>> build_environment_block(std::span<const EnvVar> env) {
    std::vector<const EnvVar*> sorted;
    sorted.reserve(env.size());
    std::size_t total = 2;
    for (const auto& var : env) {
        if (var.name.empty() || var.name.find(L'=', 1) != std::wstring::npos || var.name.find(L'\0') != std::wstring::npos ||
            var.value.find(L'\0') != std::wstring::npos) {
            return std::unexpected(ProcessError{.code = ERROR_BAD_ENVIRONMENT,
                                                .message = "invalid environment variable: " + core::wide_to_utf8(var.name)});
        }
        sorted.push_back(&var);
        total += var.name.size() + var.value.size() + 2;
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const EnvVar* a, const EnvVar* b) { return icompare(a->name, b->name) < 0; });
    std::vector<wchar_t> block;
    block.reserve(total);
    for (const EnvVar* var : sorted) {
        block.insert(block.end(), var->name.begin(), var->name.end());
        block.push_back(L'=');
        block.insert(block.end(), var->value.begin(), var->value.end());
        block.push_back(L'\0');
    }
    if (block.empty()) {
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

ProcessResult<std::wstring> resolve_executable(std::wstring_view name, const ResolveOptions& options) {
    namespace fs = std::filesystem;
    if (name.empty() || name.find(L'\0') != std::wstring_view::npos || name.find(L'"') != std::wstring_view::npos) {
        return std::unexpected(ProcessError{.code = ERROR_BAD_ARGUMENTS, .message = "invalid program name"});
    }
    std::vector<std::wstring> extensions;
    if (has_extension(name)) {
        extensions.emplace_back();
    }
    extensions.emplace_back(L".com");
    extensions.emplace_back(L".exe");
    if (options.allow_batch_files) {
        extensions.emplace_back(L".bat");
        extensions.emplace_back(L".cmd");
    }
    const auto accept = [&](const std::wstring& candidate) {
        return is_file(candidate) && (options.allow_batch_files || !is_batch_file(candidate));
    };
    const auto try_base = [&](const std::wstring& base) -> std::optional<std::wstring> {
        for (const auto& ext : extensions) {
            std::wstring candidate = base + ext;
            if (accept(candidate)) {
                std::error_code ec;
                const fs::path absolute = fs::absolute(fs::path(candidate), ec);
                return ec ? candidate : absolute.lexically_normal().wstring();
            }
        }
        return std::nullopt;
    };

    if (name.find_first_of(L"\\/:") != std::wstring_view::npos) {
        fs::path path(name);
        if (!path.is_absolute()) {
            const fs::path base = options.cwd.empty() ? fs::current_path() : fs::path(options.cwd);
            path = base / path;
        }
        if (auto found = try_base(path.lexically_normal().wstring())) {
            return *found;
        }
        return std::unexpected(make_error(ERROR_FILE_NOT_FOUND, "resolve_executable: " + core::wide_to_utf8(name)));
    }

    std::vector<std::wstring> dirs;
    const std::wstring path_env = options.path.empty() ? env_value(L"PATH") : options.path;
    std::size_t start = 0;
    while (start <= path_env.size()) {
        std::size_t end = path_env.find(L';', start);
        if (end == std::wstring::npos) {
            end = path_env.size();
        }
        std::wstring_view entry = std::wstring_view(path_env).substr(start, end - start);
        if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"') {
            entry = entry.substr(1, entry.size() - 2);
        }
        if (!entry.empty()) {
            dirs.emplace_back(entry);
        }
        start = end + 1;
    }
    if (std::wstring sys = directory_of(&get_system_dir); !sys.empty()) {
        dirs.push_back(std::move(sys));
    }
    if (std::wstring win = directory_of(&get_windows_dir); !win.empty()) {
        dirs.push_back(std::move(win));
    }
    for (auto& dir : dirs) {
        if (dir.back() != L'\\' && dir.back() != L'/') {
            dir.push_back(L'\\');
        }
        if (fs::path(dir).is_relative()) {
            continue; // a relative PATH entry would mean searching the current directory
        }
        if (auto found = try_base(dir + std::wstring(name))) {
            return *found;
        }
    }
    return std::unexpected(make_error(ERROR_FILE_NOT_FOUND, "resolve_executable: " + core::wide_to_utf8(name) + " not found in PATH"));
}

} // namespace boson::process
