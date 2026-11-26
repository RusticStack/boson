// Tauri v2 reference app of the comparison suite (bench/compare). Same commands and timeline as the other frameworks.
// Commands are async so they run on Tauri's async runtime and never occupy the main (UI) thread.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::Serialize;
use std::{env, fs, path::PathBuf};
use tauri::{AppHandle, Emitter, WebviewUrl, WebviewWindow, WebviewWindowBuilder};

#[derive(Serialize)]
struct Item {
    id: u32,
    name: String,
    value: f64,
    active: bool,
}

fn write_atomically(name: &str, text: &str) {
    let Some(dir) = env::var_os("COMPARE_DIR") else { return };
    let target = PathBuf::from(dir).join(name);
    let tmp = target.with_extension("tmp");
    if fs::write(&tmp, text).is_ok() {
        let _ = fs::rename(&tmp, &target);
    }
}

#[tauri::command]
async fn config() -> String {
    env::var("COMPARE_CONFIG").unwrap_or_else(|_| "{}".to_string())
}

#[tauri::command]
async fn ping(seq: u64) -> u64 {
    seq
}

#[tauri::command]
async fn echo(text: String) -> String {
    text
}

#[tauri::command]
async fn items() -> Vec<Item> {
    (0..10_000u32)
        .map(|id| Item { id, name: format!("item-{id}"), value: f64::from(id) * 0.5, active: id % 2 == 0 })
        .collect()
}

#[tauri::command]
async fn burst(window: WebviewWindow, n: u32) {
    let label = window.label().to_string(); // like Boson/Electron: only the window that asked gets the events
    for i in 0..n {
        let _ = window.emit_to(label.as_str(), "burst", i);
    }
}

#[tauri::command]
async fn open_window(app: AppHandle) -> Result<(), String> {
    WebviewWindowBuilder::new(&app, "w2", WebviewUrl::App("index.html?w=2".into()))
        .title("Comparison app (2)")
        .inner_size(900.0, 640.0)
        .build()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

#[tauri::command]
async fn mark(text: String) {
    if let Some((name, value)) = text.split_once(' ') {
        if !name.is_empty() && name.len() <= 32 && name.bytes().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'_') {
            write_atomically(&format!("{name}.mark"), value);
        }
    }
}

#[tauri::command]
async fn report(json: String) {
    write_atomically("result.json", &json);
    write_atomically("done.mark", "1");
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![config, ping, echo, items, burst, open_window, mark, report])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
