#pragma once

#include <boson/core/result.hpp>
#include <boson/ipc/controllers/shell_common.hpp>
#include <boson/ipc/export.hpp>
#include <boson/shell/app_id.hpp>
#include <boson/shell/auto_launch.hpp>

#include <glaze/glaze.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace boson::ipc {

struct AutoLaunchEnableRequest {
    std::vector<std::string> args;
};

struct AutoLaunchStatusResponse {
    bool registered = false;
    bool enabled = false;
    bool disabled_by_user = false;
    std::string executable;
    std::vector<std::string> args;
};

/// @brief Launch-at-login through HKCU\...\Run. The value name and executable are fixed by C++ (the
/// frontend can only toggle and choose arguments), so a page cannot register arbitrary programs.
class AutoLaunchController {
public:
    BOSON_CONTROLLER("AutoLaunch");

    /// @param name Run value name (e.g. "MyCompany.MyApp").
    explicit AutoLaunchController(std::wstring name) : m_name(std::move(name)) {
        if (m_name.empty()) {
            throw std::invalid_argument("AutoLaunchController needs a value name");
        }
    }

    [[nodiscard]] AutoLaunchStatusResponse status() const { return to_response(shell::auto_launch_status(m_name)); }

    [[nodiscard]] AutoLaunchStatusResponse enable(AutoLaunchEnableRequest req) const {
        shell::AutoLaunchOptions options;
        options.name = m_name;
        for (const auto& arg : req.args) {
            options.arguments.push_back(core::utf8_to_wide(arg));
        }
        if (!shell::enable_auto_launch(options)) {
            throw std::runtime_error("Could not write the Run registry value");
        }
        return status();
    }

    [[nodiscard]] AutoLaunchStatusResponse disable() const {
        if (!shell::disable_auto_launch(m_name)) {
            throw std::runtime_error("Could not delete the Run registry value");
        }
        return status();
    }

private:
    static AutoLaunchStatusResponse to_response(const shell::AutoLaunchStatus& s) {
        AutoLaunchStatusResponse out{
            .registered = s.registered,
            .enabled = s.enabled,
            .disabled_by_user = s.disabled_by_user,
            .executable = core::wide_to_utf8(s.executable),
            .args = {},
        };
        for (const auto& arg : s.arguments) {
            out.args.push_back(core::wide_to_utf8(arg));
        }
        return out;
    }

    std::wstring m_name;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::AutoLaunchController> {
    using T = boson::ipc::AutoLaunchController;
    static constexpr auto value = glz::object(
        "status", &T::status,
        "enable", &T::enable,
        "disable", &T::disable);
};
