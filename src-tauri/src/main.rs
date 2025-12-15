#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::{
  io::{BufRead, BufReader, Write},
  path::PathBuf,
  process::{Child, ChildStdin, Command, Stdio},
  sync::{Arc, Mutex},
  thread,
};

#[cfg(target_os = "windows")]
use std::os::windows::process::CommandExt;

use tauri::{Manager, State};

#[cfg(target_os = "windows")]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

#[derive(Default, Clone)]
struct BackendSnapshot {
  input_ready: bool,
  scan_progress_raw: Option<String>,
  registered_raw: Option<String>,
  sens_applied_raw: Option<String>,
}

#[derive(Default)]
struct BackendState {
  child: Option<Child>,
  child_stdin: Option<ChildStdin>,
  attached: bool,
  snapshot: BackendSnapshot,
}

type SharedBackendState = Arc<Mutex<BackendState>>;

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(rename_all = "camelCase")]
struct UiState {
  crosshair_memory: bool,
}

#[derive(Serialize, Clone)]
struct BackendEvent {
  kind: String,
  data: serde_json::Value,
}

fn resolve_monitor_path(app: &tauri::AppHandle) -> Result<PathBuf, String> {
  // 1) Next to the Tauri executable (portable distribution).
  if let Ok(exe) = std::env::current_exe() {
    if let Some(dir) = exe.parent() {
      let candidate = dir.join("mouse_monitor.exe");
      if candidate.exists() {
        return Ok(candidate);
      }
    }
  }

  // 2) Dev workspace root (cargo tauri dev).
  #[cfg(debug_assertions)]
  {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    if let Some(root) = manifest_dir.parent() {
      let candidate = root.join("mouse_monitor.exe");
      if candidate.exists() {
        return Ok(candidate);
      }
    }
  }

  // 3) Bundled resource (if configured).
  if let Some(candidate) = app.path_resolver().resolve_resource("mouse_monitor.exe") {
    if candidate.exists() {
      return Ok(candidate);
    }
  }

  Err("could not find `mouse_monitor.exe` (build it and place it next to the app)".to_string())
}

fn resolve_ui_state_path(app: &tauri::AppHandle) -> Result<PathBuf, String> {
  // Prefer portable behavior: store next to the Tauri executable.
  if let Ok(exe) = std::env::current_exe() {
    if let Some(dir) = exe.parent() {
      return Ok(dir.join("ui_state.json"));
    }
  }

  // Fallback: app config dir (should be writable on most systems).
  if let Some(dir) = app.path_resolver().app_config_dir() {
    return Ok(dir.join("ui_state.json"));
  }

  Err("could not resolve ui state path".to_string())
}

fn spawn_monitor(app: tauri::AppHandle, state: SharedBackendState) -> Result<(), String> {
  let mut guard = state.lock().map_err(|_| "backend mutex poisoned")?;

  if let Some(child) = guard.child.as_mut() {
    match child.try_wait() {
      Ok(Some(_)) => {
        guard.child = None;
        guard.child_stdin = None;
      }
      Ok(None) => return Ok(()),
      Err(_) => return Ok(()),
    }
  }

  guard.snapshot = BackendSnapshot::default();

  let monitor_path = resolve_monitor_path(&app)?;
  let monitor_dir = monitor_path
    .parent()
    .ok_or_else(|| "monitor path has no parent directory".to_string())?
    .to_path_buf();

  let mut cmd = Command::new(&monitor_path);
  cmd.args(["--ipc"])
    .current_dir(&monitor_dir)
    .stdin(Stdio::piped())
    .stdout(Stdio::piped())
    .stderr(Stdio::null());

  #[cfg(target_os = "windows")]
  {
    cmd.creation_flags(CREATE_NO_WINDOW);
  }

  let mut child = cmd
    .spawn()
    .map_err(|e| format!("failed to start `{}`: {e}", monitor_path.display()))?;

  let stdout = child
    .stdout
    .take()
    .ok_or_else(|| "failed to capture monitor stdout".to_string())?;
  let stdin = child
    .stdin
    .take()
    .ok_or_else(|| "failed to capture monitor stdin".to_string())?;

  guard.child_stdin = Some(stdin);
  guard.child = Some(child);

  let app_for_stdout = app.clone();
  let state_for_stdout = state.clone();
  thread::spawn(move || {
    let reader = BufReader::new(stdout);
    for line in reader.lines().flatten() {
      if let Some(evt) = parse_monitor_line(&line) {
        handle_monitor_event(&app_for_stdout, &state_for_stdout, evt);
      }
    }
  });

  Ok(())
}

fn handle_monitor_event(app: &tauri::AppHandle, state: &SharedBackendState, evt: BackendEvent) {
  let should_emit = {
    let mut guard = match state.lock() {
      Ok(g) => g,
      Err(_) => return,
    };

    update_snapshot(&mut guard.snapshot, &evt);
    guard.attached
  };

  if should_emit {
    let _ = app.emit_all("backend_event", evt);
  }
}

fn update_snapshot(snapshot: &mut BackendSnapshot, evt: &BackendEvent) {
  let raw = evt
    .data
    .get("raw")
    .and_then(|v| v.as_str())
    .unwrap_or("")
    .trim()
    .to_string();

  match evt.kind.as_str() {
    "INPUT_READY" => snapshot.input_ready = true,
    "SCAN_PROGRESS" => snapshot.scan_progress_raw = Some(raw),
    "REGISTERED" => snapshot.registered_raw = Some(raw),
    "SENS_APPLIED" => snapshot.sens_applied_raw = Some(raw),
    _ => {}
  }
}

fn emit_snapshot(app: &tauri::AppHandle, snapshot: BackendSnapshot) {
  if snapshot.input_ready {
    let _ = app.emit_all(
      "backend_event",
      BackendEvent {
        kind: "INPUT_READY".to_string(),
        data: serde_json::json!({ "raw": "" }),
      },
    );
  }

  if let Some(raw) = snapshot.scan_progress_raw {
    let _ = app.emit_all(
      "backend_event",
      BackendEvent {
        kind: "SCAN_PROGRESS".to_string(),
        data: serde_json::json!({ "raw": raw }),
      },
    );
  }

  if let Some(raw) = snapshot.sens_applied_raw {
    let _ = app.emit_all(
      "backend_event",
      BackendEvent {
        kind: "SENS_APPLIED".to_string(),
        data: serde_json::json!({ "raw": raw }),
      },
    );
  }

  if let Some(raw) = snapshot.registered_raw {
    let _ = app.emit_all(
      "backend_event",
      BackendEvent {
        kind: "REGISTERED".to_string(),
        data: serde_json::json!({ "raw": raw }),
      },
    );
  }
}

fn send_cmd(state: &SharedBackendState, line: &str) -> Result<(), String> {
  let mut guard = state.lock().map_err(|_| "backend mutex poisoned")?;
  let stdin = guard
    .child_stdin
    .as_mut()
    .ok_or_else(|| "backend not running".to_string())?;
  stdin
    .write_all(line.as_bytes())
    .and_then(|_| stdin.write_all(b"\n"))
    .and_then(|_| stdin.flush())
    .map_err(|e| format!("failed to send command: {e}"))
}

fn shutdown_monitor(state: &SharedBackendState) {
  let (mut child, mut child_stdin) = {
    let mut guard = match state.lock() {
      Ok(g) => g,
      Err(_) => return,
    };

    (guard.child.take(), guard.child_stdin.take())
  };

  if let Some(stdin) = child_stdin.as_mut() {
    let _ = stdin.write_all(b"QUIT\n");
    let _ = stdin.flush();
  }

  if let Some(child) = child.as_mut() {
    for _ in 0..50 {
      match child.try_wait() {
        Ok(Some(_)) => return,
        Ok(None) => thread::sleep(std::time::Duration::from_millis(10)),
        Err(_) => break,
      }
    }

    let _ = child.kill();
    let _ = child.wait();
  }
}

fn parse_monitor_line(line: &str) -> Option<BackendEvent> {
  let trimmed = line.trim();
  if !trimmed.starts_with("EVT ") {
    return None;
  }

  let mut parts = trimmed.splitn(3, ' ');
  let _ = parts.next()?; // EVT
  let event = parts.next()?.to_string();
  let rest = parts.next().unwrap_or("").trim();

  // Minimal structured parsing; frontend can also inspect raw lines if needed.
  Some(BackendEvent {
    kind: event,
    data: serde_json::json!({ "raw": rest }),
  })
}

#[tauri::command]
fn ui_load_state(app: tauri::AppHandle) -> Result<UiState, String> {
  let path = resolve_ui_state_path(&app)?;
  let text = match std::fs::read_to_string(&path) {
    Ok(s) => s,
    Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(UiState::default()),
    Err(err) => return Err(format!("failed to read ui state: {err}")),
  };

  match serde_json::from_str::<UiState>(&text) {
    Ok(state) => Ok(state),
    Err(_) => Ok(UiState::default()),
  }
}

#[tauri::command]
fn ui_save_state(app: tauri::AppHandle, state: UiState) -> Result<(), String> {
  let path = resolve_ui_state_path(&app)?;
  let text = serde_json::to_string_pretty(&state).map_err(|e| format!("failed to serialize ui state: {e}"))?;
  std::fs::write(&path, text).map_err(|e| format!("failed to write ui state: {e}"))
}

#[tauri::command]
fn backend_init(app: tauri::AppHandle, backend: State<'_, SharedBackendState>) -> Result<(), String> {
  let state = backend.inner().clone();
  spawn_monitor(app.clone(), state.clone())?;

  let snapshot = {
    let mut guard = state.lock().map_err(|_| "backend mutex poisoned")?;
    guard.attached = true;
    guard.snapshot.clone()
  };

  emit_snapshot(&app, snapshot);
  Ok(())
}

#[tauri::command]
fn backend_set_power(backend: State<'_, SharedBackendState>, enabled: bool) -> Result<(), String> {
  if enabled {
    send_cmd(backend.inner(), "POWER ON")
  } else {
    send_cmd(backend.inner(), "POWER OFF")
  }
}

#[tauri::command]
fn backend_set_feature(backend: State<'_, SharedBackendState>, enabled: bool) -> Result<(), String> {
  if enabled {
    send_cmd(backend.inner(), "FEATURE ON")
  } else {
    send_cmd(backend.inner(), "FEATURE OFF")
  }
}

#[tauri::command]
fn backend_set_sensitivity(backend: State<'_, SharedBackendState>, value: f64) -> Result<(), String> {
  send_cmd(backend.inner(), &format!("SET_SENS {value}"))
}

#[tauri::command]
fn backend_full_reset(backend: State<'_, SharedBackendState>) -> Result<(), String> {
  send_cmd(backend.inner(), "RESET")
}

#[tauri::command]
fn backend_quit(backend: State<'_, SharedBackendState>) -> Result<(), String> {
  send_cmd(backend.inner(), "QUIT")
}

fn main() {
  tauri::Builder::default()
    .manage(Arc::new(Mutex::new(BackendState::default())))
    .setup(|app| {
      let state = app.state::<SharedBackendState>().inner().clone();
      let _ = spawn_monitor(app.handle(), state);
      Ok(())
    })
    .on_window_event(|event| {
      if let tauri::WindowEvent::CloseRequested { .. } = event.event() {
        let state = event.window().state::<SharedBackendState>().inner().clone();
        shutdown_monitor(&state);
      }
    })
    .invoke_handler(tauri::generate_handler![
      ui_load_state,
      ui_save_state,
      backend_init,
      backend_set_power,
      backend_set_feature,
      backend_set_sensitivity,
      backend_full_reset,
      backend_quit
    ])
    .run(tauri::generate_context!())
    .expect("error while running tauri application");
}
