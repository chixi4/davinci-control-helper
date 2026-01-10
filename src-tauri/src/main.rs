#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::{
  io::{BufRead, BufReader, Write},
  path::PathBuf,
  process::{Child, ChildStdin, Command, Stdio},
  sync::{Arc, Mutex},
  thread,
  time::Duration,
};

#[cfg(target_os = "windows")]
use std::os::windows::process::CommandExt;

#[cfg(target_os = "windows")]
use std::ffi::c_void;

use tauri::{Manager, State};

#[cfg(target_os = "windows")]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

#[cfg(target_os = "windows")]
use windows_sys::Win32::{
  Foundation::{CloseHandle, GetLastError, ERROR_ALREADY_EXISTS, HANDLE, HWND, RECT},
  Graphics::Dwm::DwmSetWindowAttribute,
  Graphics::Gdi::{
    CreateCompatibleDC, CreateDIBSection, DeleteDC, DeleteObject, GetDC, ReleaseDC, SelectObject,
    SetStretchBltMode, StretchBlt, BITMAPINFO, BITMAPINFOHEADER, BI_RGB, DIB_RGB_COLORS,
    HALFTONE, HBITMAP, HDC, HGDIOBJ, SRCCOPY,
  },
  System::LibraryLoader::{GetModuleHandleW, GetProcAddress},
  System::Threading::{CreateMutexW, ReleaseMutex},
  UI::WindowsAndMessaging::{
    FindWindowW, GetSystemMetrics, GetWindowRect, SetForegroundWindow, ShowWindow,
    SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN, SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN, SW_RESTORE,
  },
};

#[cfg(target_os = "windows")]
struct SingleInstanceGuard {
  handle: HANDLE,
}

#[cfg(target_os = "windows")]
impl Drop for SingleInstanceGuard {
  fn drop(&mut self) {
    if self.handle.is_null() {
      return;
    }
    unsafe {
      let _ = ReleaseMutex(self.handle);
      let _ = CloseHandle(self.handle);
    }
  }
}

#[cfg(target_os = "windows")]
fn to_wide_null_terminated(s: &str) -> Vec<u16> {
  use std::os::windows::ffi::OsStrExt;
  std::ffi::OsStr::new(s)
    .encode_wide()
    .chain(std::iter::once(0))
    .collect()
}

#[cfg(target_os = "windows")]
fn try_focus_existing_window() {
  let title = to_wide_null_terminated("RawAccel Monitor");
  unsafe {
    let hwnd = FindWindowW(std::ptr::null(), title.as_ptr());
    if !hwnd.is_null() {
      let _ = ShowWindow(hwnd, SW_RESTORE);
      let _ = SetForegroundWindow(hwnd);
    }
  }
}

#[cfg(target_os = "windows")]
fn ensure_single_instance() -> Result<SingleInstanceGuard, ()> {
  let mutex_name = to_wide_null_terminated("Local\\RawAccelMonitorGui_SingleInstance");

  unsafe {
    let handle = CreateMutexW(std::ptr::null(), 1, mutex_name.as_ptr());
    if handle.is_null() {
      return Ok(SingleInstanceGuard {
        handle: std::ptr::null_mut(),
      });
    }

    if GetLastError() == ERROR_ALREADY_EXISTS {
      let _ = CloseHandle(handle);
      try_focus_existing_window();
      return Err(());
    }

    Ok(SingleInstanceGuard { handle })
  }
}

#[cfg(target_os = "windows")]
const ACCENT_ENABLE_ACRYLICBLURBEHIND: i32 = 4;

#[cfg(target_os = "windows")]
const WCA_ACCENT_POLICY: u32 = 19;

#[cfg(target_os = "windows")]
#[repr(C)]
struct AccentPolicy {
  accent_state: i32,
  accent_flags: u32,
  gradient_color: u32,
  animation_id: u32,
}

#[cfg(target_os = "windows")]
#[repr(C)]
struct WindowCompositionAttribData {
  attrib: u32,
  data: *mut c_void,
  size: usize,
}

#[cfg(target_os = "windows")]
type SetWindowCompositionAttributeFn =
  unsafe extern "system" fn(HWND, *mut WindowCompositionAttribData) -> i32;

#[cfg(target_os = "windows")]
fn to_abgr(a: u8, r: u8, g: u8, b: u8) -> u32 {
  ((a as u32) << 24) | ((b as u32) << 16) | ((g as u32) << 8) | (r as u32)
}

#[cfg(target_os = "windows")]
fn apply_window_acrylic(window: &tauri::Window) {
  let hwnd = match window.hwnd() {
    Ok(hwnd) => hwnd.0 as isize,
    Err(_) => return,
  };

  let user32_name = to_wide_null_terminated("user32.dll");
  let user32 = unsafe { GetModuleHandleW(user32_name.as_ptr()) };
  if user32.is_null() {
    return;
  }

  let proc = unsafe { GetProcAddress(user32, b"SetWindowCompositionAttribute\0".as_ptr()) };
  let Some(proc) = proc else {
    return;
  };

  let set_window_composition_attribute: SetWindowCompositionAttributeFn =
    unsafe { std::mem::transmute(proc) };

  let mut accent = AccentPolicy {
    accent_state: ACCENT_ENABLE_ACRYLICBLURBEHIND,
    accent_flags: 0,
    gradient_color: to_abgr(50, 12, 12, 12),
    animation_id: 0,
  };

  let mut data = WindowCompositionAttribData {
    attrib: WCA_ACCENT_POLICY,
    data: &mut accent as *mut _ as *mut c_void,
    size: std::mem::size_of::<AccentPolicy>(),
  };

  unsafe {
    let _ = set_window_composition_attribute(hwnd as HWND, &mut data);
  }
}

#[cfg(target_os = "windows")]
const AMBIENT_SAMPLE_SIZE: i32 = 32;

#[cfg(target_os = "windows")]
const AMBIENT_SAMPLE_SOURCE: i32 = 96;

#[cfg(target_os = "windows")]
const AMBIENT_SAMPLE_MARGIN: i32 = 12;

#[cfg(target_os = "windows")]
const AMBIENT_SAMPLE_INTERVAL_MS: u64 = 100;

#[cfg(target_os = "windows")]
const AMBIENT_EMA_ALPHA: f32 = 0.12;

#[cfg(target_os = "windows")]
const AMBIENT_EMIT_EPSILON: f32 = 0.005;

#[cfg(target_os = "windows")]
#[derive(Copy, Clone)]
struct RectI32 {
  left: i32,
  top: i32,
  right: i32,
  bottom: i32,
}

#[cfg(target_os = "windows")]
impl RectI32 {
  fn width(&self) -> i32 {
    self.right - self.left
  }

  fn height(&self) -> i32 {
    self.bottom - self.top
  }

  fn intersects(&self, other: &RectI32) -> bool {
    self.left < other.right
      && self.right > other.left
      && self.top < other.bottom
      && self.bottom > other.top
  }
}

#[cfg(target_os = "windows")]
fn rect_from_win(rect: RECT) -> RectI32 {
  RectI32 {
    left: rect.left,
    top: rect.top,
    right: rect.right,
    bottom: rect.bottom,
  }
}

#[cfg(target_os = "windows")]
fn clamp_rect(rect: RectI32, bounds: RectI32) -> Option<RectI32> {
  let left = rect.left.max(bounds.left);
  let top = rect.top.max(bounds.top);
  let right = rect.right.min(bounds.right);
  let bottom = rect.bottom.min(bounds.bottom);
  if right <= left || bottom <= top {
    return None;
  }
  Some(RectI32 {
    left,
    top,
    right,
    bottom,
  })
}

#[cfg(target_os = "windows")]
struct AmbientSampler {
  screen_dc: HDC,
  mem_dc: HDC,
  dib: HBITMAP,
  old_obj: HGDIOBJ,
  bits: *mut u8,
  width: i32,
  height: i32,
}

#[cfg(target_os = "windows")]
impl AmbientSampler {
  unsafe fn new() -> Option<Self> {
    let screen_dc = GetDC(std::ptr::null_mut());
    if screen_dc.is_null() {
      return None;
    }

    let mem_dc = CreateCompatibleDC(screen_dc);
    if mem_dc.is_null() {
      let _ = ReleaseDC(std::ptr::null_mut(), screen_dc);
      return None;
    }

    let mut bmi: BITMAPINFO = std::mem::zeroed();
    bmi.bmiHeader = BITMAPINFOHEADER {
      biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
      biWidth: AMBIENT_SAMPLE_SIZE,
      biHeight: -AMBIENT_SAMPLE_SIZE,
      biPlanes: 1,
      biBitCount: 32,
      biCompression: BI_RGB,
      biSizeImage: 0,
      biXPelsPerMeter: 0,
      biYPelsPerMeter: 0,
      biClrUsed: 0,
      biClrImportant: 0,
    };

    let mut bits: *mut c_void = std::ptr::null_mut();
    let dib = CreateDIBSection(
      mem_dc,
      &bmi,
      DIB_RGB_COLORS,
      &mut bits,
      std::ptr::null_mut(),
      0,
    );
    if dib.is_null() || bits.is_null() {
      let _ = DeleteDC(mem_dc);
      let _ = ReleaseDC(std::ptr::null_mut(), screen_dc);
      return None;
    }

    let old_obj = SelectObject(mem_dc, dib as _);
    let _ = SetStretchBltMode(mem_dc, HALFTONE);

    Some(Self {
      screen_dc,
      mem_dc,
      dib,
      old_obj,
      bits: bits as *mut u8,
      width: AMBIENT_SAMPLE_SIZE,
      height: AMBIENT_SAMPLE_SIZE,
    })
  }

  unsafe fn capture_brightness(&mut self, rect: RectI32) -> Option<f32> {
    let source_w = rect.width();
    let source_h = rect.height();
    if source_w <= 0 || source_h <= 0 {
      return None;
    }

    let ok = StretchBlt(
      self.mem_dc,
      0,
      0,
      self.width,
      self.height,
      self.screen_dc,
      rect.left,
      rect.top,
      source_w,
      source_h,
      SRCCOPY,
    );
    if ok == 0 {
      return None;
    }

    if self.bits.is_null() {
      return None;
    }

    let total = (self.width * self.height) as usize;
    let mut sum = 0.0f32;
    let mut offset = 0usize;
    for _ in 0..total {
      let b = *self.bits.add(offset) as f32;
      let g = *self.bits.add(offset + 1) as f32;
      let r = *self.bits.add(offset + 2) as f32;
      sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
      offset += 4;
    }

    let avg = sum / (total as f32 * 255.0);
    Some(avg.clamp(0.0, 1.0))
  }
}

#[cfg(target_os = "windows")]
impl Drop for AmbientSampler {
  fn drop(&mut self) {
    unsafe {
      if !self.mem_dc.is_null() && !self.old_obj.is_null() {
        let _ = SelectObject(self.mem_dc, self.old_obj);
      }
      if !self.dib.is_null() {
        let _ = DeleteObject(self.dib as _);
      }
      if !self.mem_dc.is_null() {
        let _ = DeleteDC(self.mem_dc);
      }
      if !self.screen_dc.is_null() {
        let _ = ReleaseDC(std::ptr::null_mut(), self.screen_dc);
      }
    }
  }
}

#[cfg(target_os = "windows")]
fn sample_window_brightness(hwnd: HWND, sampler: &mut AmbientSampler) -> Option<f32> {
  let mut rect = RECT {
    left: 0,
    top: 0,
    right: 0,
    bottom: 0,
  };

  let ok = unsafe { GetWindowRect(hwnd, &mut rect) };
  if ok == 0 {
    return None;
  }

  let window = rect_from_win(rect);
  if window.width() <= 0 || window.height() <= 0 {
    return None;
  }

  let v_left = unsafe { GetSystemMetrics(SM_XVIRTUALSCREEN) };
  let v_top = unsafe { GetSystemMetrics(SM_YVIRTUALSCREEN) };
  let v_width = unsafe { GetSystemMetrics(SM_CXVIRTUALSCREEN) };
  let v_height = unsafe { GetSystemMetrics(SM_CYVIRTUALSCREEN) };
  if v_width <= 0 || v_height <= 0 {
    return None;
  }

  let bounds = RectI32 {
    left: v_left,
    top: v_top,
    right: v_left + v_width,
    bottom: v_top + v_height,
  };

  let half = AMBIENT_SAMPLE_SOURCE / 2;
  let quarter_w = window.width() / 4;
  let quarter_h = window.height() / 4;
  let x_points = [
    window.left + quarter_w,
    window.left + window.width() / 2,
    window.right - quarter_w,
  ];
  let y_points = [
    window.top + quarter_h,
    window.top + window.height() / 2,
    window.bottom - quarter_h,
  ];

  let mut candidates = Vec::with_capacity(12);
  for x in x_points {
    candidates.push(RectI32 {
      left: x - half,
      top: window.top - AMBIENT_SAMPLE_MARGIN - AMBIENT_SAMPLE_SOURCE,
      right: x + half,
      bottom: window.top - AMBIENT_SAMPLE_MARGIN,
    });
    candidates.push(RectI32 {
      left: x - half,
      top: window.bottom + AMBIENT_SAMPLE_MARGIN,
      right: x + half,
      bottom: window.bottom + AMBIENT_SAMPLE_MARGIN + AMBIENT_SAMPLE_SOURCE,
    });
  }
  for y in y_points {
    candidates.push(RectI32 {
      left: window.left - AMBIENT_SAMPLE_MARGIN - AMBIENT_SAMPLE_SOURCE,
      top: y - half,
      right: window.left - AMBIENT_SAMPLE_MARGIN,
      bottom: y + half,
    });
    candidates.push(RectI32 {
      left: window.right + AMBIENT_SAMPLE_MARGIN,
      top: y - half,
      right: window.right + AMBIENT_SAMPLE_MARGIN + AMBIENT_SAMPLE_SOURCE,
      bottom: y + half,
    });
  }

  let mut sum = 0.0f32;
  let mut count = 0u32;
  for rect in candidates {
    let Some(clamped) = clamp_rect(rect, bounds) else {
      continue;
    };
    if clamped.intersects(&window) {
      continue;
    }
    let Some(value) = (unsafe { sampler.capture_brightness(clamped) }) else {
      continue;
    };
    sum += value;
    count += 1;
  }

  if count == 0 {
    None
  } else {
    Some(sum / count as f32)
  }
}

#[cfg(target_os = "windows")]
fn start_ambient_sampler(app: tauri::AppHandle, hwnd: isize) {
  thread::spawn(move || {
    let mut sampler = match unsafe { AmbientSampler::new() } {
      Some(sampler) => sampler,
      None => return,
    };
    let mut smooth = 1.0f32;
    let mut last_emit = smooth;
    let hwnd = hwnd as HWND;

    loop {
      if let Some(sample) = sample_window_brightness(hwnd, &mut sampler) {
        smooth += (sample - smooth) * AMBIENT_EMA_ALPHA;
        smooth = smooth.clamp(0.0, 1.0);
      }

      if (smooth - last_emit).abs() >= AMBIENT_EMIT_EPSILON {
        if app.emit_all("ambient-brightness", smooth).is_err() {
          break;
        }
        last_emit = smooth;
      }

      thread::sleep(Duration::from_millis(AMBIENT_SAMPLE_INTERVAL_MS));
    }
  });
}

#[cfg(target_os = "windows")]
const DWMWA_WINDOW_CORNER_PREFERENCE: u32 = 33;

#[cfg(target_os = "windows")]
const DWMWCP_ROUND: u32 = 2;

#[cfg(target_os = "windows")]
fn apply_window_rounding(window: &tauri::Window) {
  let hwnd = match window.hwnd() {
    Ok(hwnd) => hwnd.0 as isize,
    Err(_) => return,
  };

  let preference: u32 = DWMWCP_ROUND;
  unsafe {
    let _ = DwmSetWindowAttribute(
      hwnd as HWND,
      DWMWA_WINDOW_CORNER_PREFERENCE,
      &preference as *const _ as *const c_void,
      std::mem::size_of::<u32>() as u32,
    );
  }
}
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
      let candidates = [
        dir.join("backend").join("mouse_monitor.exe"),
        dir.join("mouse_monitor.exe"),
      ];
      for candidate in candidates {
        if candidate.exists() {
          return Ok(candidate);
        }
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

  Err(
    "could not find `mouse_monitor.exe` (build it and place it next to the app, or next to the app in `backend/`)".to_string(),
  )
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
  #[cfg(target_os = "windows")]
  let _single_instance_guard = match ensure_single_instance() {
    Ok(g) => g,
    Err(()) => return,
  };

  tauri::Builder::default()
    .manage(Arc::new(Mutex::new(BackendState::default())))
    .setup(|app| {
      let state = app.state::<SharedBackendState>().inner().clone();
      let _ = spawn_monitor(app.handle(), state);
      #[cfg(target_os = "windows")]
      {
        if let Some(window) = app.get_window("main") {
          apply_window_acrylic(&window);
          if let Ok(hwnd) = window.hwnd() {
            start_ambient_sampler(app.handle(), hwnd.0 as isize);
          }
          apply_window_rounding(&window);
          if let Ok(hwnd) = window.hwnd() {
            start_ambient_sampler(app.handle(), hwnd.0 as isize);
          }
        }
      }
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
