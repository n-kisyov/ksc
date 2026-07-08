# KSC - Keystroke Counter

```
  ██╗  ██╗███████╗ ██████╗
  ██║ ██╔╝██╔════╝██╔════╝
  █████╔╝ ███████╗██║
  ██╔═██╗ ╚════██║██║
  ██║  ██╗███████║╚██████╗
  ╚═╝  ╚═╝╚══════╝ ╚═════╝
     Keystroke Counter
```

A lightweight Windows application written in modern C that counts every keystroke on your keyboard and stores the statistics in a SQLite database.

<p align="center">
  <tt>████████  ████  ████████  ████  ████████  ████</tt><br>
  <tt>██  ████  ████  ██  ████  ████  ██  ████  ████</tt><br>
  <tt>████  ███  ████    ██  ██  ██  ██    ██  ██</tt>
</p>

## Features

- **Keystroke counting** - Captures every keypress system-wide via low-level keyboard hook
- **SQLite storage** - Persists counts per-key in `%APPDATA%\KSC\ksc.db`
- **Stats view** - ListView sorted by count descending (most-clicked first)
- **Auto-refresh** - Configurable 10-second auto-refresh (default: ON)
- **Dark mode** - Full dark theme including title bar, list view, scrollbar, and menu bar
- **System tray** - Minimizes to tray on close/minimize; right-click for Show/Settings/Quit
- **Start minimized** - Option to launch directly to system tray
- **Start with Windows** - Auto-launch via registry toggle
- **Single instance** - Prevents duplicate processes via named mutex
- **Custom app icon** - Programmatic icon (no external files required)
- **Static linking** - Standalone `.exe` with zero runtime DLL dependencies
- **Low footprint** - Minimal CPU and memory usage

## Settings

The settings dialog (File > Settings) provides four toggles:

| Setting | Default | Description |
|---|---|---|
| Start with Windows | OFF | Launches KSC on Windows login (registry Run key) |
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
3. Configure and build the project with static linking
4. Copy `ksc.exe` to the project root

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
│   ├── keyhook.c/h         WH_KEYBOARD_LL hook in dedicated thread
│   ├── database.c/h        SQLite operations + settings table
│   ├── gui.c/h             Main window, list view, settings dialog, dark mode
│   ├── tray.c/h            System tray icon + right-click menu
│   ├── startup.c/h         Registry auto-start toggle
│   └── ksc_private.h       Shared constants and includes
└── sqlite3/                SQLite3 amalgamation (downloaded by build.ps1)
```

## How It Works

### Keyboard Hook

Uses `SetWindowsHookEx(WH_KEYBOARD_LL)` to capture all keystrokes system-wide. The hook runs in a dedicated thread with its own message pump, ensuring no impact on GUI responsiveness. Each key-down event (excluding repeats) increments a counter in the database.

### Database

Counts are stored in `%APPDATA%\KSC\ksc.db` with an upsert pattern:

```sql
key_counts        settings
──────────        ────────
key_code  INT     key    TEXT  (PK)
key_name  TEXT    value  TEXT
count     INT
```

The `settings` table stores user preferences (dark mode, auto-refresh, etc.).

### Dark Mode

Dark mode is implemented via undocumented `uxtheme.dll` ordinal exports loaded at runtime:

- `SetPreferredAppMode(AllowDark)` (ordinal 135) - enables dark mode support
- `AllowDarkModeForWindow` (ordinal 133) - opts individual windows into dark chrome
- `FlushMenuThemes` (ordinal 136) - refreshes menu rendering

Combined with `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` for the title bar and `SetWindowTheme(hWnd, L"DarkMode_Explorer", NULL)` for the ListView's scrollbar. Falls back gracefully to manual `ListView_SetBkColor` on older Windows builds.

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
| Linking | Fully static (no DLLs required) |
| Theme | uxtheme + DWM APIs |

## Future Plans

- Mouse button click tracking
- Daily/weekly/monthly statistics
- Export stats to CSV
- Per-application key tracking
- Heatmap visualization

## License

MIT License - feel free to use and modify.

---

Built with ❤️ for Windows 11
