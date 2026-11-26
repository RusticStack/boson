#include <boson/process/policy.hpp>

#include <boson/core/result.hpp>
#include <boson/security/fs_scope.hpp>

#include <glaze/glaze.hpp>

#include <windows.h>

#include <algorithm>
#include <filesystem>

namespace boson::process {

namespace {

[[nodiscard]] bool iequals(std::wstring_view a, std::wstring_view b) noexcept {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool istarts_with(std::wstring_view text, std::wstring_view prefix) noexcept {
    return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
}

/// Final path of an existing object without the "\\?\" prefix; the lexically normalised absolute path when
/// it does not exist (yet).
[[nodiscard]] std::wstring canonical(std::wstring_view path) {
    if (auto final_path = security::FsScope::final_path_of(path)) {
        return security::FsScope::display_form(*final_path);
    }
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::path(path), ec);
    return ec ? std::wstring(path) : absolute.lexically_normal().wstring();
}

[[nodiscard]] ProcessError denied(std::string message) {
    return ProcessError{.code = ERROR_ACCESS_DENIED, .message = "permission denied: " + std::move(message)};
}

[[nodiscard]] std::wstring strip_exe(std::wstring_view name) {
    if (name.size() > 4 && iequals(name.substr(name.size() - 4), L".exe")) {
        name.remove_suffix(4);
    }
    return std::wstring(name);
}

} // namespace

ProcessResult<std::shared_ptr<const ExecutablePolicy>> ExecutablePolicy::create(ExecutablePolicyConfig config, ArgumentFilter filter,
                                                                               std::shared_ptr<const security::FsScope> cwd_scope) {
    auto policy = std::make_shared<ExecutablePolicy>();
    for (const auto& exe : config.executables) {
        const std::wstring wide = core::utf8_to_wide(exe);
        if (wide.empty() || std::filesystem::path(wide).is_relative()) {
            return std::unexpected(ProcessError{.code = ERROR_BAD_PATHNAME, .message = "ExecutablePolicy: executable must be absolute: " + exe});
        }
        policy->m_executables.push_back(canonical(wide));
    }
    for (const auto& dir : config.directories) {
        const std::wstring wide = core::utf8_to_wide(dir);
        if (wide.empty() || std::filesystem::path(wide).is_relative()) {
            return std::unexpected(ProcessError{.code = ERROR_BAD_PATHNAME, .message = "ExecutablePolicy: directory must be absolute: " + dir});
        }
        std::wstring c = canonical(wide);
        if (!c.empty() && c.back() != L'\\') {
            c.push_back(L'\\');
        }
        policy->m_directories.push_back(std::move(c));
    }
    for (const auto& name : config.commands) {
        const std::wstring wide = core::utf8_to_wide(name);
        if (wide.empty() || wide.find_first_of(L"\\/:") != std::wstring::npos) {
            return std::unexpected(ProcessError{.code = ERROR_BAD_ARGUMENTS, .message = "ExecutablePolicy: command must be a bare name: " + name});
        }
        policy->m_commands.push_back(strip_exe(wide));
    }
    policy->m_config = std::move(config);
    policy->m_filter = std::move(filter);
    policy->m_cwd_scope = std::move(cwd_scope);
    return std::shared_ptr<const ExecutablePolicy>(std::move(policy));
}

ProcessResult<std::shared_ptr<const ExecutablePolicy>> ExecutablePolicy::from_json(std::string_view json) {
    ExecutablePolicyConfig config;
    const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true}>(config, json);
    if (ec) {
        return std::unexpected(ProcessError{.code = ERROR_INVALID_DATA, .message = "ExecutablePolicy: " + glz::format_error(ec, json)});
    }
    return create(std::move(config));
}

std::shared_ptr<const ExecutablePolicy> ExecutablePolicy::deny_all() {
    return std::make_shared<const ExecutablePolicy>();
}

ProcessResult<std::wstring> ExecutablePolicy::authorize(std::string_view command, std::span<const std::string> args) const {
    const std::wstring wide = core::utf8_to_wide(command);
    if (wide.empty()) {
        return std::unexpected(denied("empty command"));
    }
    std::wstring resolved;
    bool allowed = false;
    if (wide.find_first_of(L"\\/:") == std::wstring::npos) {
        const std::wstring bare = strip_exe(wide);
        const bool listed = std::any_of(m_commands.begin(), m_commands.end(), [&](const std::wstring& c) { return iequals(c, bare); });
        if (!listed) {
            return std::unexpected(denied("'" + std::string(command) + "' is not an allowed command"));
        }
        auto found = resolve_executable(wide, ResolveOptions{.path = {}, .cwd = {}, .allow_batch_files = m_config.allow_batch_files});
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        resolved = canonical(*found);
        allowed = true;
    } else {
        if (std::filesystem::path(wide).is_relative()) {
            return std::unexpected(denied("program paths must be absolute: " + std::string(command)));
        }
        auto found = resolve_executable(wide, ResolveOptions{.path = {}, .cwd = {}, .allow_batch_files = m_config.allow_batch_files});
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        resolved = canonical(*found);
        allowed = std::any_of(m_executables.begin(), m_executables.end(), [&](const std::wstring& e) { return iequals(e, resolved); });
        if (!allowed) {
            for (const auto& dir : m_directories) {
                if (!istarts_with(resolved, dir)) {
                    continue;
                }
                const std::wstring_view rest = std::wstring_view(resolved).substr(dir.size());
                if (m_config.recursive_directories || rest.find(L'\\') == std::wstring_view::npos) {
                    allowed = true;
                    break;
                }
            }
        }
    }
    if (!allowed) {
        return std::unexpected(denied(core::wide_to_utf8(resolved) + " is not in the executable allow-list"));
    }
    if (is_batch_file(resolved) && !m_config.allow_batch_files) {
        return std::unexpected(denied("batch files are not allowed"));
    }
    if (m_filter && !m_filter(resolved, args)) {
        return std::unexpected(denied("arguments rejected for " + core::wide_to_utf8(resolved)));
    }
    return resolved;
}

ProcessResult<std::string> ExecutablePolicy::authorize_cwd(std::string_view cwd) const {
    if (cwd.empty()) {
        return std::string{};
    }
    if (!m_config.allow_cwd) {
        return std::unexpected(denied("cwd is not allowed"));
    }
    if (m_cwd_scope) {
        auto resolved = m_cwd_scope->resolve(cwd, security::FsAccess::Read);
        if (!resolved) {
            return std::unexpected(denied("cwd outside the allowed scope: " + resolved.error().message));
        }
        return resolved->display;
    }
    const std::wstring wide = core::utf8_to_wide(cwd);
    if (std::filesystem::path(wide).is_relative()) {
        return std::unexpected(denied("cwd must be absolute"));
    }
    return core::wide_to_utf8(canonical(wide));
}

} // namespace boson::process
