# Boson Developer Usage Guide

This guide walks through building a full Windows desktop application with **Boson**.

---

## 1. Project Structure

```text
Boson/
├── CMakeLists.txt
├── cmake/
│   ├── EmbedAssets.cmake      # Generates constexpr asset fallback when #embed is unavailable
│   └── FetchWebView2.cmake    # Downloads Microsoft.Web.WebView2 NuGet & exposes WebView2::Static
├── include/boson/
│   ├── boson.hpp              # Umbrella header
│   ├── assets/asset_registry.hpp
│   ├── core/                  # Window, WebView, ProtocolHandler, inplace_vector, result
│   ├── exec/                  # Scheduler, TaskRunner, UiMarshal
│   └── ipc/                   # export, protocol_types, router, bridge, ts_generator
├── src/                       # Core implementations
├── examples/demo/             # Full demo app (AppController + wWinMain)
├── tools/tsgen/               # CLI tool for generating boson-bindings.d.ts
├── frontend/                  # Vite + TypeScript dashboard
└── tests/                     # CTest unit test suite
```

---

## 2. Integrating `boson_core` in CMake

Link your `WIN32` executable against `boson_core`:

```cmake
add_executable(my_app WIN32
    src/main.cpp
)
target_link_libraries(my_app PRIVATE boson_core)
```

`boson_core` automatically propagates:
- `/std:c++latest /Zc:__cplusplus /permissive- /EHsc /bigobj /utf-8 /W4`
- `NOMINMAX`, `WIN32_LEAN_AND_MEAN`, `UNICODE`, `_UNICODE`
- `glaze::glaze`, `STDEXEC::stdexec`, `WebView2::Static`, and required Win32/COM system libraries (`shlwapi`, `dwmapi`, `ole32`, `version`, etc.)
- Release LTO (`CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE`) and linker stripping (`/OPT:REF`, `/OPT:ICF`, `/DEBUG:NONE`).

---

## 3. Defining C++ IPC Controllers

A controller is a plain C++ class/struct marked with `BOSON_CONTROLLER("Name")` and a `glz::meta<T>` specialization listing exported methods:

```cpp
#include <boson/ipc/export.hpp>
#include <glaze/glaze.hpp>
#include <stdexcept>
#include <string>

struct GreetRequest {
    std::string name;
};

struct GreetResponse {
    std::string message;
};

class GreeterController {
public:
    BOSON_CONTROLLER("Greeter");

    [[nodiscard]] GreetResponse hello(GreetRequest req) const {
        if (req.name.empty()) {
            throw std::invalid_argument("name must not be empty");
        }
        return GreetResponse{.message = "Hello, " + req.name + "!"};
    }

    void ping() const {}
};

template <>
struct glz::meta<GreeterController> {
    using T = GreeterController;
    static constexpr auto value = glz::object(
        "hello", &T::hello,
        "ping", &T::ping
    );
};
```

### Supported Method Signatures

- `Ret method()` — 0 arguments, returns JSON-serializable `Ret`
- `Ret method(Arg)` — 1 Glaze-reflectable struct or primitive argument, returns `Ret`
- `void method()` — 0 arguments, returns `null` / `Promise<void>`
- `void method(Arg)` — 1 argument, returns `Promise<void>`

Both `const` and non-`const` member functions are supported.

### Exception & Error Handling

Any C++ exception thrown inside a controller method is caught by `boson::exec::TaskRunner` before crossing the COM/ABI boundary and returned to JavaScript as:

```json
{
  "id": 1,
  "status": "error",
  "error": {
    "code": -32000,
    "message": "name must not be empty"
  }
}
```

On the frontend, `boson.call(...)` rejects the Promise with a `BosonIpcError(message, code)`.

---

## 4. Wiring `Window`, `WebView`, `TaskRunner`, and `Bridge`

In `wWinMain`:

```cpp
#include <windows.h>
#include <objbase.h>
#include <boson/boson.hpp>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    boson::core::WindowConfig win_cfg;
    win_cfg.title = L"My Boson App";
    win_cfg.width = 1280;
    win_cfg.height = 800;
    win_cfg.dark_mode = true;
    win_cfg.borderless = false; // set true for custom titlebar apps

    boson::core::Window window(std::move(win_cfg));
    if (!window.create(instance, show_cmd)) {
        return 1;
    }

    boson::exec::Scheduler scheduler;
    boson::exec::UiMarshal marshal([&window]() { window.post_invoke(); });
    window.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });

    boson::exec::TaskRunner runner(scheduler, marshal);

    GreeterController greeter;
    boson::ipc::Router<GreeterController> router(greeter);

    boson::core::WebViewConfig wv_cfg;
    wv_cfg.initial_url = "boson://app/index.html";
    // wv_cfg.dev_url = "http://localhost:5173"; // enable for Vite HMR

    boson::core::WebView webview(std::move(wv_cfg));
    boson::ipc::Bridge bridge(webview, runner, marshal, router);

    window.set_resize_callback([&webview](int w, int h) { webview.resize(w, h); });

    if (!webview.initialize(
            window.hwnd(),
            [](std::string_view path) { return boson::assets::AssetRegistry::find(path); },
            bridge.make_message_callback(),
            [&window, &webview]() {
                const RECT rc = window.client_rect();
                webview.resize(rc.right - rc.left, rc.bottom - rc.top);
            })) {
        return 1;
    }

    const int exit_code = window.run_message_loop();
    ::CoUninitialize();
    return exit_code;
}
```

---

## 5. Generating TypeScript Bindings

`boson::ipc::TsGenerator::generate<Controllers...>()` inspects your reflected controllers and emits a `.d.ts` file containing all request/response interfaces and the `BosonApi` map:

```cpp
#include <boson/ipc/ts_generator.hpp>
std::string dts = boson::ipc::TsGenerator::generate<GreeterController>();
```

Build and run `boson_tsgen`:

```powershell
.\build\release\boson_tsgen.exe frontend\src\generated\boson-bindings.d.ts
```

Then call your backend from TypeScript with full type safety:

```ts
import { boson, BosonIpcError } from './boson';
import type { GreetRequest, GreetResponse } from './generated/boson-bindings';

try {
  const res = await boson.call<GreetResponse, GreetRequest>('Greeter.hello', { name: 'Boson' });
  console.log(res.message);
} catch (err) {
  if (err instanceof BosonIpcError) {
    console.error(`IPC Error ${err.code}: ${err.message}`);
  }
}
```

---

## 6. Asset Bundling & Vite HMR

- **Production (`boson://app/*`)**: Build `frontend/dist/` (`bun run build`) before configuring CMake. `cmake/EmbedAssets.cmake` packages all files in `frontend/dist/` into read-only `constexpr` byte arrays (or uses `#embed` when supported). `ProtocolHandler` serves them directly from memory via `SHCreateMemStream` with zero disk I/O.
- **Development (HMR)**: Start Vite (`cd frontend && bun run dev`) and launch the app with `--dev` (defaults to `http://localhost:5173`) or `--dev http://localhost:PORT` / `BOSON_DEV_URL`.

---

## 7. Production Release Builds

```powershell
cd frontend && bun run build && cd ..
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/release'
```

This produces a single statically linked executable (`WebView2LoaderStatic.lib` embedded, no `WebView2Loader.dll` needed) with LTO and debug symbols stripped.
