#pragma once

#include <functional>
#include <string_view>

#include "boson/core/webview.hpp"
#include "boson/exec/task_runner.hpp"
#include "boson/exec/ui_marshal.hpp"
#include "boson/ipc/export.hpp"
#include "boson/ipc/router.hpp"

namespace boson::ipc {

/// @brief Connects WebView2 messaging, TaskRunner execution, UiMarshal delivery, and a type-erased Router.
class Bridge {
public:
    using DispatchCallback = std::function<void(std::string_view)>;

    /// @brief Constructs a Bridge with a type-erased router dispatch callback.
    Bridge(
        core::WebView& webview,
        exec::TaskRunner& runner,
        exec::UiMarshal& marshal,
        DispatchCallback dispatch_fn);

    /// @brief Convenience constructor binding directly to a typed Router<Controllers...>.
    template <BosonController... Controllers>
    Bridge(
        core::WebView& webview,
        exec::TaskRunner& runner,
        exec::UiMarshal& marshal,
        Router<Controllers...>& router)
        : Bridge(
              webview,
              runner,
              marshal,
              [&router, &runner](std::string_view raw_json) {
                  router.dispatch_async(raw_json, runner);
              }) {}

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    Bridge(Bridge&&) = delete;
    Bridge& operator=(Bridge&&) = delete;

    /// @brief Detaches the UI delivery callback on destruction.
    ~Bridge();

    /// @brief Routes an incoming JSON-RPC message from WebView2 through the dispatch callback.
    void on_web_message(std::string_view raw_json) const;

    /// @brief Returns a WebView::MessageCallback forwarding incoming messages to this Bridge.
    [[nodiscard]] core::WebView::MessageCallback make_message_callback() const;

private:
    core::WebView& m_webview;
    exec::TaskRunner& m_runner;
    exec::UiMarshal& m_marshal;
    DispatchCallback m_dispatch_fn;
};

}  // namespace boson::ipc
