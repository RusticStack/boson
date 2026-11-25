#include "boson/ipc/bridge.hpp"

#include <utility>

namespace boson::ipc {

Bridge::Bridge(
    core::WebView& webview,
    exec::TaskRunner& runner,
    exec::UiMarshal& marshal,
    DispatchCallback dispatch_fn)
    : m_webview(webview),
      m_runner(runner),
      m_marshal(marshal),
      m_dispatch_fn(std::move(dispatch_fn)) {
    m_marshal.set_deliver_callback([this](std::string_view json_payload) {
        m_webview.post_web_message_as_json(json_payload);
    });
}

Bridge::~Bridge() {
    m_marshal.set_deliver_callback({});
}

void Bridge::on_web_message(std::string_view raw_json) const {
    if (m_dispatch_fn) {
        m_dispatch_fn(raw_json);
    }
}

core::WebView::MessageCallback Bridge::make_message_callback() const {
    return [this](std::string_view raw_json) {
        on_web_message(raw_json);
    };
}

}  // namespace boson::ipc
