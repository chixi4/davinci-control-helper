# Tauri GUI setup

This repo now contains a **Tauri + Vite + React** GUI:

- Frontend: `src/` (the UI from `前端界面3.txt` is now in `src/App.jsx`)
- Tauri backend: `src-tauri/` (spawns `mouse_monitor.exe --ipc` and bridges events/commands)

## Prereqs (Windows)

- Node.js (18+ recommended)
- Rust toolchain (stable)
- MSVC Build Tools or MinGW (to build `mouse_monitor.cpp`)
- WebView2 Runtime (required by Tauri on Windows)

## Prepare `settings.json`

`mouse_monitor` reads/writes `settings.json` and calls `writer.exe` to apply changes.

1. Copy `settings.example.json` to `settings.json` in the same folder as `mouse_monitor.exe`
2. Make sure RawAccel is installed/running so `writer.exe` can apply settings

## Build `mouse_monitor.exe`

Run:

```powershell
./build.bat
```

Make sure these files exist in the repo root:

- `mouse_monitor.exe`
- `writer.exe`
- `wrapper.dll`
- `settings.json`

## Run the GUI (dev)

```powershell
npm i
npm run tauri:dev
```

## Build the GUI (portable)

This builds the GUI **without installers** (portable binary):

```powershell
npm run tauri:build
```

After the build, the script copies `mouse_monitor.exe`, `writer.exe`, `wrapper.dll` (and creates `settings.json` if missing) into:

- `src-tauri/target/release/`

It also produces a clean portable folder:

- `dist-portable/RawAccel Monitor/`

Run:

- `dist-portable/RawAccel Monitor/RawAccel Monitor.exe`

If you want installer bundles instead, run:

```powershell
npm run tauri:bundle
```

## Portable distribution note

This setup is **portable**: at runtime the GUI looks for `mouse_monitor.exe` next to the Tauri app executable, and `mouse_monitor.exe` looks for `settings.json` next to itself.

So when you run the built app from a folder, keep these together in the same directory:

- `RawAccel Monitor.exe` (the Tauri app)
- `mouse_monitor.exe`
- `writer.exe`
- `wrapper.dll`
- `settings.json`

## IPC protocol (debug)

You can run the backend directly:

```powershell
./mouse_monitor.exe --ipc
```

Commands (one per line, via stdin):

- `POWER ON` / `POWER OFF`
- `FEATURE ON` / `FEATURE OFF`
- `SET_SENS <value>` (0.001 ~ 100)
- `RESET`
- `QUIT`

Events (stdout lines starting with `EVT `):

- `EVT READY`
- `EVT SCAN_PROGRESS <percent>`
- `EVT REGISTERED <hardwareId>`
- `EVT POWER ON|OFF`
- `EVT FEATURE ON|OFF`
- `EVT FIRING ON|OFF`
- `EVT NOTIFY OK:...` / `EVT NOTIFY ERR:...` / `EVT NOTIFY FS:LOST|CONNECTING|OFFLINE`
