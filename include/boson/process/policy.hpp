#pragma once

#include <boson/process/command_line.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::security {
class FsScope;
}

namespace boson::process {

/// @brief What pages may start through the Process controller. Everything not listed is denied.
struct ExecutablePolicyConfig {
    /// Exact programs (absolute paths, resolved to their final path: symlinks / junctions followed).
    std::vector<std::string> executables;
    /// Folders whose programs (and sub-folders' when recursive) may run, e.g. the app's bundled sidecars.
    std::vector<std::string> directories;
    bool recursive_directories = true;
    /// Bare names ("git", "rg", "node") looked up in this process's PATH at spawn time. A page can never
    /// change the PATH used for this lookup (a PATH in the request's env only reaches the child).
    std::vector<std::string> commands;
    /// .bat / .cmd run through cmd.exe; even escaped, a script can re-expand its arguments. Off by default.
    bool allow_batch_files = false;
    bool allow_detached = false;
    /// Pages may pass env (and clear_env).
    bool allow_env = true;
    /// Pages may choose cwd. When cwd_scope is set, cwd must also resolve inside it.
    bool allow_cwd = true;
    std::uint32_t max_processes = 32;
};

/// @brief Compiled allow-list. authorize() resolves what a page asked for to the absolute path that will be
/// run (the controller spawns exactly that path) or denies with ERROR_ACCESS_DENIED.
class ExecutablePolicy {
public:
    /// Extra veto on top of the lists (exe = resolved path, args = UTF-8 arguments).
    using ArgumentFilter = std::function<bool(std::wstring_view exe, std::span<const std::string> args)>;

    ExecutablePolicy() = default; ///< denies everything
    [[nodiscard]] static ProcessResult<std::shared_ptr<const ExecutablePolicy>> create(ExecutablePolicyConfig config,
                                                                                       ArgumentFilter filter = {},
                                                                                       std::shared_ptr<const security::FsScope> cwd_scope = {});
    /// @brief {"executables":[..],"directories":[..],"commands":[..],"allow_batch_files":false,...}
    [[nodiscard]] static ProcessResult<std::shared_ptr<const ExecutablePolicy>> from_json(std::string_view json);
    [[nodiscard]] static std::shared_ptr<const ExecutablePolicy> deny_all();

    /// @brief The absolute program path for command, or ERROR_ACCESS_DENIED / ERROR_FILE_NOT_FOUND.
    [[nodiscard]] ProcessResult<std::wstring> authorize(std::string_view command, std::span<const std::string> args) const;
    /// @brief The cwd to use (absolute) or an error; "" stays "".
    [[nodiscard]] ProcessResult<std::string> authorize_cwd(std::string_view cwd) const;

    [[nodiscard]] const ExecutablePolicyConfig& config() const noexcept { return m_config; }

private:
    ExecutablePolicyConfig m_config;
    std::vector<std::wstring> m_executables; ///< final paths without "\\?\"
    std::vector<std::wstring> m_directories; ///< final paths with a trailing '\'
    std::vector<std::wstring> m_commands;
    ArgumentFilter m_filter;
    std::shared_ptr<const security::FsScope> m_cwd_scope;
};

} // namespace boson::process
