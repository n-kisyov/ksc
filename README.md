# ksc - Keystroke Counter

```
  ██╗  ██╗███████╗ ██████╗
  ██║ ██╔╝██╔════╝██╔════╝
  █████╔╝ ███████╗██║
  ██╔═██╗ ╚════██║██║
  ██║  ██╗███████║╚██████╗
  ╚═╝  ╚═╝╚══════╝ ╚═════╝
     Keystroke Counter v0.9
```

A lightweight Windows application written in modern C that counts every keystroke on your keyboard, tracks per-application usage, and stores statistics in a SQLite database.

<p align="center">
  <tt>████████  ████  ████████  ████  ████████  ████</tt><br>
  <tt>██  ████  ████  ██  ████  ████  ██  ████  ████</tt><br>
  <tt>████  ███  ████    ██  ██  ██  ██    ██  ██</tt>
</p>

## Features

- **Keystroke & mouse counting** - Captures keyboard keys and left/right mouse clicks system-wide via low-level hooks
- **Per-application tracking** - Records which app received each keypress (foreground window title)
- **SQLite storage** - Persists counts in `%APPDATA%\KSC\ksc.db` with per-key and per-key-per-app-per-day tables
- **Stats view** - ListView sorted by count descending (most-clicked first), with optional auto-refresh
- **Key heatmap** - Visual keyboard layout colored by usage frequency (blue → red gradient)
- **Date-range statistics** - Custom date range selector with From/To date pickers and per-app filtering
- **CSV export** - Export all-time or date-filtered data to CSV, including per-app breakdown
- **Dark mode** - Full dark theme including title bar, menu bar, list view, scrollbar, and all child windows
- **System tray** - Minimizes to tray on close/minimize; right-click for Show/Heatmap/Stats/Settings/Quit
- **Start minimized** - Option to launch directly to system tray
- **Start with Windows** - Auto-launch via registry toggle
- **Single instance** - Prevents duplicate processes via named mutex
- **Self-signed code signing** - Authenticode signature with timestamp at build time; one-time trust command eliminates SmartScreen warnings
- **Custom app icon** - Embedded icon resource (no external files required at runtime)
- **Static linking** - Standalone `.exe` with zero runtime DLL dependencies
- **Low footprint** - Minimal CPU and memory usage

## Settings

The settings dialog (File > Settings) provides four toggles:

| Setting | Default | Description |
|---|---|---|
| Start with Windows | OFF | Launches ksc on Windows login (registry Run key) |
| Start minimized to tray | OFF | Starts hidden in the system tray |
| Dark mode | OFF | Full dark theme for all window elements |
| Auto-refresh stats (10s) | ON | Refreshes the stats view every 10 seconds |

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
│   ├── main.c              Entry point, mutex, message loop
│   ├── keyhook.c/h         WH_KEYBOARD_LL + WH_MOUSE_LL hooks, per-app capture
│   ├── database.c/h        SQLite operations, settings table, daily/app tracking
│   ├── gui.c/h             Main window, heatmap, stats window, settings, dark mode
│   ├── tray.c/h            System tray icon + right-click menu
│   ├── startup.c/h         Registry auto-start toggle
│   ├── ksc.rc              Embedded icon + VERSIONINFO resource
│   ├── resource.h          Resource IDs
│   └── ksc_private.h       Shared constants and includes
└── sqlite3/                SQLite3 amalgamation (downloaded by build.ps1)
```

## How It Works

### Hook System

Uses two low-level Windows hooks in a dedicated thread with its own message pump:
- `SetWindowsHookEx(WH_KEYBOARD_LL)` — captures all keystrokes system-wide
- `SetWindowsHookEx(WH_MOUSE_LL)` — captures left and right mouse button clicks

Each event captures the foreground window title via `GetForegroundWindow()` + `GetWindowText()` for per-app tracking. Only non-repeating key-down and button-down events are recorded.

### Database

Counts are stored in `%APPDATA%\KSC\ksc.db` with three tables:

```sql
key_counts          key_daily                     settings
──────────          ────────                      ────────
key_code  INT  PK   key_code  INT              key    TEXT  PK
key_name  TEXT      date      TEXT             value  TEXT
count     INT       app       TEXT   DEFAULT ''
                    count     INT
                    PRIMARY KEY (key_code, date, app)
```

- `key_counts` — all-time totals per key
- `key_daily` — per-key-per-app-per-day granular counts for time-range and per-app queries
- `settings` — user preferences (dark mode, auto-refresh, etc.)

All inserts use upsert (`INSERT ... ON CONFLICT DO UPDATE`) with `SQLITE_OPEN_FULLMUTEX` for thread safety between the hook thread and the GUI thread.

### Key Heatmap

A separate window draws a full QWERTY keyboard layout (~70 keys). Each key rectangle is colored on a dark-blue → teal → green → yellow → orange → red gradient based on its press count relative to the most-pressed key. A legend bar at the bottom shows the scale.

### Statistics & Export

The Stats window provides:
- **From/To date pickers** — select any custom date range
- **App filter dropdown** — populated from `SELECT DISTINCT app FROM key_daily`, defaults to "All apps"
- **CSV Export** — exports `Key Code,Key Name,App,Count` for the selected date range and app filter

The File > Export Data menu item exports all-time data (from `2000-01-01` to today) with full per-app breakdown.

### Dark Mode

Dark mode is implemented via undocumented `uxtheme.dll` ordinal exports loaded at runtime:

- `SetPreferredAppMode(AllowDark)` (ordinal 135) — enables dark mode support app-wide
- `AllowDarkModeForWindow` (ordinal 133) — opts individual windows into dark chrome (title bar, scrollbar)
- `FlushMenuThemes` (ordinal 136) — refreshes menu rendering

Combined with `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` for the title bar and `SetWindowTheme(hWnd, L"DarkMode_Explorer", NULL)` for the ListView's dark scrollbar. Item coloring uses `NM_CUSTOMDRAW` for guaranteed dark appearance regardless of theme support.

### Single Instance

A named mutex (`KSC_SingleInstance`) is created at startup. If the mutex already exists, the new process exits silently, preventing duplicate instances.

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

## License

MIT License — feel free to use and modify.

---

Built with ❤️ for Windows 11
