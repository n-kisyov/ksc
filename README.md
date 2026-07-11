# ksc - Keystroke Counter

**Version 0.9.5rc1** — A lightweight Windows application written in modern C (~2.5k LOC) that counts every keystroke on your keyboard, tracks per-application usage, includes a keylogger, mouse clicker, heatmap, statistics, and stores everything in SQLite databases.

## Features

### Core
- **Keystroke & mouse counting** — Captures keyboard keys and left/right mouse clicks system-wide via low-level hooks (`WH_KEYBOARD_LL` + `WH_MOUSE_LL`)
- **Per-application tracking** — Records which app (foreground window title) received each keypress
- **Total counters** — Main window shows live separated totals: keyboard keypresses vs mouse clicks
- **Batch database writes** — Ring buffer (256 events) + dedicated writer threads flush in `BEGIN/COMMIT` transactions, minimising SQLite overhead

### Windows & Views
- **Key heatmap** — Full QWERTY keyboard layout colored by usage frequency (blue → red gradient) with legend
- **Statistics window** — From/To date pickers, app filter dropdown, keyboard/mouse separated totals, auto-refreshes every 10s, CSV export
- **Keylogger log viewer** — View keylogger records filtered by date range and app in a themed ListView
- **Mouse clicker** — Configurable interval (min/sec/ms), random offset, left/right button, continuous or limited mode, system-wide start/stop hotkeys with customisable bindings
- **Dark mode** — Full dark theme including title bar, menu bar, list view, scrollbar, and all child windows (heatmap, stats, logs, clicker, settings, about)

### System Integration
- **System tray** — Minimises to tray on close/minimize; right-click for Show/Heatmap/Stats/Mouse Clicker/Toggle Keylogger/Settings/Quit
- **Tray tooltip** — Shows today's total keypress count, updates every 10s
- **Show/Hide hotkey** — Configurable system-wide shortcut (default Ctrl+Shift+K) to bring ksc to foreground when minimised or hidden
- **Start minimised** — Option to launch directly to system tray
- **Start with Windows** — Auto-launch via registry `Run` key toggle
- **Single instance** — Named mutex prevents duplicate processes

### Data Management
- **CSV export** — All-time or date-filtered export with per-app breakdown
- **Backup database** — Timestamped backup of `ksc.db` and `ksc_keylog.db` with one click
- **Restore database** — Select a backup file to restore; app exits for clean restart
- **Reset statistics** — Wipe all keypress/mouse data while preserving settings
- **Delete keylogger logs** — Erase the keylogger database independently

### Build & Distribution
- **Self-signed code signing** — Authenticode signature with timestamp at build time; one-time trust command eliminates SmartScreen warnings
- **Custom app icon** — Embedded icon resource, shown in all windows and Explorer
- **Static linking** — Standalone `.exe` with zero runtime DLL dependencies
- **VERSIONINFO resource** — File properties show version, company, product name

## Settings

The settings dialog (File > Settings) provides:

| Setting | Default | Description |
|---|---|---|
| Start with Windows | OFF | Launches ksc on Windows login (registry Run key) |
| Start minimised to tray | OFF | Starts hidden in the system tray |
| Dark mode | OFF | Full dark theme for all windows and components |
| Auto-refresh stats (10s) | ON | Refreshes stats view every 10 seconds |
| Enable Keylogger | OFF | Records all keypresses with timestamps to `ksc_keylog.db` |
| Reset All Statistics | — | Deletes all keypress counts, keeps settings |
| Delete Keylogger Logs | — | Deletes the keylogger database file |
| Show KSC Shortcut | Ctrl+Shift+K | Configurable system-wide hotkey to show/hide |

All settings persist in the SQLite database.

## Requirements

- Windows 10+ (Windows 11 recommended)
- **Build requirements:**
  - CMake 3.15+
  - GCC (MinGW-w64)
  - Internet connection (to download SQLite amalgamation on first build)

## Quick Start

### Automated Build

```powershell
.\build.ps1
```

The script will:
1. Check that CMake and GCC are available
2. Download the SQLite3 amalgamation (if not present)
3. Generate the application icon
4. Configure and build the project with static linking
5. Self-sign the executable with a timestamp
6. Copy `ksc.exe` to the project root
7. Export the public certificate to `ksc.cer` and print the trust command
8. Show source line counts (app + sqlite3)

After the first build, run the trust command once (as Admin) to eliminate SmartScreen warnings:
```powershell
Import-Certificate -FilePath .\ksc.cer -CertStoreLocation Cert:\CurrentUser\TrustedPublisher
```

### Manual Build

1. Download the [SQLite amalgamation](https://www.sqlite.org/download.html) and place `sqlite3.c` and `sqlite3.h` in the `sqlite3/` directory.
2. Build:

```bat
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

## Project Structure

```
ksc/
├── CMakeLists.txt          Build configuration (static linking, C11)
├── build.ps1               Automated build script (PowerShell)
├── README.md               This file
├── src/
│   ├── main.c              Entry point, mutex, hotkey registration, message loop
│   ├── keyhook.c/h         WH_KEYBOARD_LL + WH_MOUSE_LL hooks, per-app capture, keylogger toggle
│   ├── database.c/h        SQLite ops, settings, daily/app tracking, batch writer thread
│   ├── keylogdb.c/h        Separate keylogger database, batch writer thread
│   ├── gui.c/h             All windows: main, settings, heatmap, stats, logs, clicker, dark mode
│   ├── tray.c/h            System tray, tooltip, right-click menu
│   ├── startup.c/h         Registry auto-start toggle
│   ├── ksc.rc              Embedded icon + VERSIONINFO resource
│   ├── resource.h          Resource IDs
│   └── ksc_private.h       Shared constants and includes
└── sqlite3/                SQLite3 amalgamation (downloaded by build.ps1)
```

## Architecture

### Hook System

Two low-level Windows hooks run in a dedicated thread with their own message pump:
- `SetWindowsHookEx(WH_KEYBOARD_LL)` — captures all keystrokes system-wide
- `SetWindowsHookEx(WH_MOUSE_LL)` — captures left/right mouse button clicks

Each event captures the foreground window title via `GetForegroundWindow()` + `GetWindowText()`. Events are pushed to a lock-free ring buffer (critical section protected) and flushed in batches by a background writer thread.

### Database

Two independent SQLite databases in `%APPDATA%\KSC\`:

**ksc.db** — statistics and settings:
```sql
key_counts          key_daily                     settings
──────────          ────────                      ────────
key_code  INT  PK   key_code  INT              key    TEXT  PK
key_name  TEXT      date      TEXT             value  TEXT
count     INT       app       TEXT   DEFAULT ''
                    count     INT
                    PRIMARY KEY (key_code, date, app)
```

**ksc_keylog.db** — keylogger records:
```sql
keylog
──────
id         INTEGER PRIMARY KEY AUTOINCREMENT
timestamp  TEXT     (YYYY-MM-DD HH:MM:SS)
key_name   TEXT
vk_code    INTEGER
app        TEXT     DEFAULT ''
```

### Batch Write System

Each keystroke pushes an event to a 256-entry ring buffer. A dedicated writer thread wakes every ~100ms or on signal, drains the buffer inside a `BEGIN IMMEDIATE / COMMIT` transaction, and performs all upserts in a single SQLite journal write. This reduces per-event overhead from ~0.5ms to ~0.01ms amortised.

### Dark Mode

Dark mode is implemented via undocumented `uxtheme.dll` ordinal exports loaded at runtime:
- `SetPreferredAppMode(AllowDark)` (ordinal 135) — enables dark mode support app-wide
- `AllowDarkModeForWindow` (ordinal 133) — opts individual windows into dark chrome
- `FlushMenuThemes` (ordinal 136) — refreshes menu rendering
- `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` — dark title bar
- `SetWindowTheme(hWnd, L"DarkMode_Explorer", NULL)` — dark ListView scrollbar
- `NM_CUSTOMDRAW` — guaranteed dark item text/background

### System-Wide Hotkeys

Three `RegisterHotKey`-based hotkeys processed via `WM_HOTKEY` in the main message loop:
- **Show/Hide ksc** (default Ctrl+Shift+K)
- **Start clicking** (default Ctrl+Shift+S)
- **Stop clicking** (default Ctrl+Shift+X)

All three work system-wide — including when minimised, in the tray, or over fullscreen applications.

## Tech Stack

| Component | Technology |
|---|---|
| Language | C11 |
| Compiler | GCC (MinGW-w64) |
| Build system | CMake |
| Database | SQLite3 (amalgamation, statically linked) |
| UI toolkit | Win32 API (no frameworks) |
| Code signing | Self-signed Authenticode via PowerShell |
| Linking | Fully static (no DLLs required) |
| Theme | uxtheme + DWM APIs |

## Future Development Goals

- **Keyboard simulator** — companion to the mouse clicker; configurable key sequences with repeat and interval
- **Session tracking** — detect app-switch boundaries, view sessions with duration and keypress counts
- **Idle detection** — pause counting after N minutes of inactivity to reduce noise
- **Portable mode** — detect `ksc.portable` file; store databases alongside the exe for USB use
- **Per-app heatmap** — filter the heatmap by application
- **Typing speed display** — approximate live WPM based on recent keystrokes
- **Daily/weekly email reports** — scheduled CSV export and summary
- **Split gui.c** — refactor monolithic 2.5k-line file into per-window modules
- **Linux support** — replace Win32 hooks/UI with platform-appropriate equivalents (X11/Wayland)

## License

MIT License — feel free to use and modify.

---

Built with ❤️ for Windows 11
