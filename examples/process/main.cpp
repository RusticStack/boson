// examples/process/main.cpp: boson::process from C++ (no window).
//   1. exec(): run a program to completion and collect its output (no shell; arguments are quoted for you).
//   2. spawn(): stream line output from a long-running child, write to its stdin, kill its tree.
//   3. Sidecar: a Node helper (bundled sidecars/node/node.exe, else node on PATH) supervised with a ready
//      line, an allocated loopback port, restart-on-failure and a graceful stop.
#include <windows.h>

#include <boson/process/process.hpp>
#include <boson/process/sidecar.hpp>
#include <boson/core/result.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace bp = boson::process;
using namespace std::chrono_literals;

namespace {

void demo_exec(bp::ProcessManager& pm) {
    const auto t0 = std::chrono::steady_clock::now();
    auto r = pm.exec_sync({.spawn = {.file = "cmd", .args = {"/d", "/c", "echo", "hello", "from", "cmd.exe"}}});
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!r) {
        std::cout << "exec failed: " << r.error().message << '\n';
        return;
    }
    std::cout << "[exec] code " << r->status.exit_code << " in " << ms << " ms: " << r->stdout_data;
}

void demo_stream(bp::ProcessManager& pm) {
    // sort.exe reads stdin to the end, then prints the sorted lines.
    auto p = pm.spawn({.file = "sort",
                       .output_mode = bp::OutputMode::Line,
                       .on_lines = [](int fd, std::span<const std::string_view> lines) {
                           for (const auto line : lines) {
                               std::cout << "[sort fd " << fd << "] " << line << '\n';
                           }
                       }});
    if (!p) {
        std::cout << "spawn failed: " << p.error().message << '\n';
        return;
    }
    (*p)->write(std::string_view("pear\r\napple\r\nfig\r\n"));
    (*p)->close_stdin();
    if (const auto status = (*p)->wait(5s)) {
        std::cout << "[sort] exited " << status->exit_code << " after " << status->run_time.count() << " ms\n";
    }
}

void demo_sidecar(bp::ProcessManager& pm) {
    std::filesystem::path node = bp::bundled_sidecar_directory("node") / L"node.exe";
    if (!std::filesystem::exists(node)) {
        auto found = bp::resolve_executable(L"node");
        if (!found) {
            std::cout << "[sidecar] node.exe not bundled and not on PATH: skipped\n";
            return;
        }
        node = *found;
    }
    // A tiny HTTP server on the port Boson allocated; exits when stdin closes (the graceful stop).
    const std::string script =
        "const http = require('http');"
        "const port = Number(process.env.BOSON_SIDECAR_PORT);"
        "http.createServer((q, s) => s.end('hi from node ' + process.version)).listen(port, '127.0.0.1',"
        "  () => console.log('READY ' + port));"
        "process.stdin.on('end', () => { console.log('bye'); process.exit(0); }); process.stdin.resume();";
    bp::Sidecar sidecar(pm, {.name = "node-http",
                             .spawn = {.file = boson::core::wide_to_utf8(node.wstring()), .args = {"-e", script}},
                             .restart = {.mode = bp::RestartMode::OnFailure, .max_restarts = 3},
                             .ready = {.kind = bp::ReadyKind::LineContains, .pattern = "READY"},
                             .stop = {.method = bp::StopMethod::CloseStdin, .grace = 2s},
                             .allocate_port = true,
                             .on_state = [](bp::SidecarState s) { std::cout << "[sidecar] state " << bp::to_string(s) << '\n'; }});
    sidecar.start();
    if (const auto ready = sidecar.wait_ready(15s)) {
        std::cout << "[sidecar] ready in " << ready->startup_time.count() << " ms on 127.0.0.1:" << ready->port
                  << " (pid " << ready->pid << ")\n";
        auto curl = pm.exec_sync({.spawn = {.file = "curl", .args = {"-s", "http://127.0.0.1:" + std::to_string(ready->port) + "/"}}});
        if (curl && curl->status.exit_code == 0) {
            std::cout << "[sidecar] GET / -> " << curl->stdout_data << '\n';
        }
    }
    sidecar.stop_and_wait(5s);
}

} // namespace

int main() {
    bp::ProcessManager pm; // children join a kill-on-close job: none outlives this process
    demo_exec(pm);
    demo_stream(pm);
    demo_sidecar(pm);
    return 0;
}
