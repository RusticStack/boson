#pragma once

#include <boson/core/result.hpp>
#include <boson/desktop/shell.hpp>
#include <boson/ipc/export.hpp>

#include <glaze/glaze.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace boson::ipc {

struct ShellUrlRequest {
    std::string url;
};

struct ShellPathRequest {
    std::string path;
};

struct ShellFolderRequest {
    std::string name; ///< "home" | "desktop" | "documents" | "downloads" | ... (see desktop::parse_known_folder)
};

struct ShellPathResponse {
    std::string path;
};

/// @brief JS "Shell.*". The shell calls block while handlers start, which is fine on the pool thread.
class ShellController {
public:
    BOSON_CONTROLLER("Shell");

    explicit ShellController(desktop::OpenExternalOptions options = {})
        : m_options(std::move(options)) {}

    /// Only schemes in OpenExternalOptions::allowed_schemes (http, https, mailto by default).
    void open_external(ShellUrlRequest req) const { check(desktop::open_external(req.url, m_options)); }
    void open_path(ShellPathRequest req) const { check(desktop::open_path(to_path(req.path))); }
    void show_item_in_folder(ShellPathRequest req) const { check(desktop::show_item_in_folder(to_path(req.path))); }
    void move_to_trash(ShellPathRequest req) const { check(desktop::move_to_trash(to_path(req.path))); }

    [[nodiscard]] ShellPathResponse get_known_folder(ShellFolderRequest req) const {
        const auto folder = desktop::parse_known_folder(req.name);
        if (!folder) {
            throw std::invalid_argument("unknown folder name: " + req.name);
        }
        auto path = desktop::known_folder(*folder);
        if (!path) {
            throw std::runtime_error(path.error());
        }
        return ShellPathResponse{.path = core::wide_to_utf8(path->wstring())};
    }

private:
    static std::filesystem::path to_path(const std::string& utf8) {
        if (utf8.empty()) {
            throw std::invalid_argument("path is empty");
        }
        return std::filesystem::path(core::utf8_to_wide(utf8));
    }

    static void check(const desktop::ShellResult<void>& result) {
        if (!result) {
            throw std::runtime_error(result.error());
        }
    }

    desktop::OpenExternalOptions m_options;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::ShellController> {
    using T = boson::ipc::ShellController;
    static constexpr auto value = glz::object(
        "open_external", &T::open_external,
        "open_path", &T::open_path,
        "show_item_in_folder", &T::show_item_in_folder,
        "move_to_trash", &T::move_to_trash,
        "get_known_folder", &T::get_known_folder);
};
