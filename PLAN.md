# Boson: The C++26 Webview Framework

**Vision:** A zero-overhead, developer-friendly C++ alternative to Tauri and Wails. By leveraging modern C++26 features (static reflection, senders/receivers, `#embed`) and AI-assisted boilerplate generation, Boson provides a seamless TypeScript-to-C++ experience.

**Initial Target:** Windows 10/11 (WebView2 / Win32 COM).

---

## Phase 1: The Bare-Metal Windows Foundation
*Goal: Bootstrap a Win32 window hosting Edge WebView2 with minimal dependencies.*

**Why AI is crucial here:** Win32 and COM (Component Object Model) APIs are notoriously verbose. AI can instantly generate the boilerplate needed to initialize `ICoreWebView2Environment` and attach it to an `HWND` without you needing to memorize HRESULT error codes.

*   **Step 1.1: Build System Setup**
    *   Use modern **CMake** with C++26 explicitly enabled (`set(CMAKE_CXX_STANDARD 26)`).
    *   Integrate `Microsoft.Web.WebView2` via NuGet (using CMake's external project tools) or vcpkg.
*   **Step 1.2: Window Initialization**
    *   Create a clean, object-oriented `Window` class encapsulating the `HWND` and Win32 Message Loop.
    *   Implement DPI awareness, dark mode title bars (DWMWA_USE_IMMERSIVE_DARK_MODE), and borderless window capabilities.
*   **Step 1.3: WebView2 Bootstrap**
    *   Implement the asynchronous WebView2 initialization sequence.
    *   Inject a basic hardcoded HTML string to verify rendering and layout resizing.

## Phase 2: The C++26 Compile-Time IPC Bridge (The "Magic")
*Goal: Eliminate binding boilerplate using C++26 Static Reflection (P2996).*

This is where Boson differentiates itself from legacy C++ libraries. Instead of manual string-matching or macros, we reflect over C++ structs.

*   **Step 2.1: Define the API Contract**
    *   Create a C++ concept/attribute system to mark classes/methods as "Frontend Accessible" (e.g., `struct [[boson::export]] AppController`).
*   **Step 2.2: The Reflection Router**
    *   Use C++26 static reflection (`^^` and `[: :]`) to iterate over the methods of a user-provided struct at compile time.
    *   Automatically generate a static hash map (or nested `if constexpr` tree) that routes an incoming string (e.g., `"App.GetSystemInfo"`) to the correct function pointer.
*   **Step 2.3: WebView2 Message Handling**
    *   Bind to `ICoreWebView2::add_WebMessageReceived`.
    *   Parse incoming JSON payloads using a high-performance library like `glaze` or `simdjson`.
    *   Feed the parsed JSON into the Reflection Router to trigger the C++ function.
    *   Serialize the return value back to JSON and send it via `ICoreWebView2::PostWebMessageAsJson`.

## Phase 3: Asynchronous Task Routing (Non-Blocking Native)
*Goal: Prevent C++ operations from blocking the Windows UI thread, mapping JS Promises directly to C++ concurrency.*

*   **Step 3.1: Thread Pools & Senders/Receivers**
    *   Utilize C++26 `std::execution` (Senders/Receivers) to route incoming IPC requests to a background thread pool.
    *   When the background task completes, safely marshal the result back to the main Win32 thread (using a custom Win32 message like `WM_APP + 1`) to call the WebView2 post-message API.
*   **Step 3.2: Promise Resolution**
    *   Standardize the JSON envelope. Every request from the frontend includes a unique `callback_id`.
    *   The C++ backend executes asynchronously and replies with `{ "id": callback_id, "status": "success", "data": ... }`, seamlessly resolving the frontend TypeScript Promise.

## Phase 4: Developer Experience (DX) & TypeScript Interop
*Goal: Make building a Boson app feel as smooth as modern web development.*

*   **Step 4.1: Automated TypeScript Generation**
    *   Since C++26 reflection knows the exact signatures (names, parameters, return types) of your backend functions, write a compile-time pass that outputs a `boson-bindings.d.ts` file. 
    *   *Result:* 100% type-safe RPC calls from frontend to backend without manual definition files.
*   **Step 4.2: Local Development Server (HMR)**
    *   Implement a debug mode where Boson loads a local Vite dev server URL (e.g., `http://localhost:5173`) instead of compiled assets, allowing instant frontend Hot Module Replacement (HMR) while maintaining native C++ connectivity.

## Phase 5: Packaging & Asset Bundling
*Goal: Ship a single, self-contained, lightning-fast `.exe` file.*

*   **Step 5.1: C++ `#embed` for Web Assets**
    *   Use the C++23/26 `#embed` directive to pull the compiled frontend (HTML, CSS, JS) directly into the executable's read-only data segment at compile time.
*   **Step 5.2: Custom Protocol Handler**
    *   Intercept WebView2 navigation using `ICoreWebView2::AddWebResourceRequestedFilter`.
    *   Serve the embedded files from memory using a custom scheme like `boson://app/index.html`. This ensures zero disk I/O for asset loading and protects source code.
*   **Step 5.3: Release Stripping**
    *   Configure CMake to compile with LTO (Link Time Optimization) and strip debug symbols to produce an executable that competes with (or beats) Rust/Tauri in size and startup time.