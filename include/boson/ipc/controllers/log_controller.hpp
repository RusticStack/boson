#pragma once

#include <boson/ipc/export.hpp>
#include <boson/shipping/logger.hpp>

#include <glaze/glaze.hpp>

#include <memory>
#include <string>
#include <utility>

namespace boson::ipc {

struct LogWriteRequest {
    std::string level = "info";
    std::string category = "web";
    std::string message;
};

/// @brief JS: Log.write({level, category, message}) appends to the app's rotating log file; the category is
/// prefixed with "web:" so frontend lines are easy to tell apart. Messages are capped at 16 KiB.
class LogController {
public:
    BOSON_CONTROLLER("Log");

    explicit LogController(std::shared_ptr<shipping::Logger> logger)
        : m_logger(std::move(logger)) {}

    void write(LogWriteRequest req) const {
        constexpr std::size_t kMaxMessage = 16 * 1024;
        if (req.message.size() > kMaxMessage) {
            req.message.resize(kMaxMessage);
            req.message += "...";
        }
        if (req.category.size() > 64) {
            req.category.resize(64);
        }
        shipping::log_message(
            m_logger,
            shipping::parse_log_level(req.level, shipping::LogLevel::Info),
            "web:" + req.category,
            req.message);
    }

private:
    std::shared_ptr<shipping::Logger> m_logger;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::LogController> {
    using T = boson::ipc::LogController;
    static constexpr auto value = glz::object("write", &T::write);
};
