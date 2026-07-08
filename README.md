# KSC - Keystroke Counter

A lightweight Windows application written in modern C that counts every keystroke on your keyboard and stores the statistics in a SQLite database.

## Features

- Counts each keystroke on the keyboard (system-wide)
- Stores counts in a local SQLite database
- Minimal GUI with sortable stats (most-clicked to least-clicked)
- System tray integration with right-click menu
- Minimize to tray (close button hides to tray)
- Auto-start with Windows toggle (via registry)
- Statically linked executable - no runtime DLLs needed
- Low resource footprint

## Screenshots

*(Add screenshots here after building)*

## Requirements

- Windows 11 (should also work on Windows 7+)
- **Build requirements:**
  - CMake 3.15+
  - GCC (MinGW-w64)
  - Internet connection (to download SQLite amalgamation on first build)

## Quick Start

### Option 1: Automated Build

```bat
build.bat
```

This script will:
1. Check for CMake and GCC
2. Download the SQLite3 amalgamation (if not present)
3. Configure and build the project
4. Copy `ksc.exe` to the project root

### Option 2: Manual Build

1. Download [SQLite amalgamation](https://www.sqlite.org/download.html) and place `sqlite3.c` and `sqlite3.h` in the `sqlite3/` directory.

2. Build with CMake:

```bat
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

3. The executable `ksc.exe` will be in the `build/` directory.

## Project Structure

```
ksc/
+-- CMakeLists.txt          Build configuration
+-- build.bat               Automated build script
+-- README.md               This file
+-- src/
    +-- main.c              Application entry point
    +-- keyhook.c/h         Low-level keyboard hook
    +-- database.c/h        SQLite database operations
    +-- gui.c/h             Main window and settings UI
    +-- tray.c/h            System tray integration
    +-- startup.c/h         Auto-start registry toggle
    +-- ksc_private.h       Shared defines and includes
+-- sqlite3/                SQLite3 amalgamation (downloaded by build.bat)
```

## How It Works

1. **Keyboard Hook** - Uses `SetWindowsHookEx` with `WH_KEYBOARD_LL` to capture all keystrokes system-wide. The hook runs in its own thread with a dedicated message pump.

2. **Database** - Each keypress increments a counter in a SQLite database stored at `%APPDATA%\KSC\ksc.db`. The schema maps virtual key codes to human-readable names and counts.

3. **GUI** - A Win32 window with a list view showing all tracked keys sorted by count (descending). The view auto-refreshes every second.

4. **System Tray** - The app minimizes to the system tray when:
   - The close button is clicked
   - The window is minimized
   
   Right-clicking the tray icon shows a menu with:
   - **Show/Hide** - Toggle the main window
   - **Settings** - Configure auto-start
   - **Quit** - Exit the application

5. **Auto-Start** - When enabled in Settings, the app adds itself to `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` in the registry.

## Settings

- **Start with Windows** - When checked, KSC will automatically launch when you log into Windows. Toggle via File > Settings.

## Tech Stack

- **Language:** C11
- **Compiler:** GCC (MinGW-w64)
- **Build System:** CMake
- **Database:** SQLite3 (amalgamation)
- **UI:** Win32 API
- **Linking:** Static (no DLLs required)

## Future Plans

- Mouse button click tracking
- Daily/weekly/monthly statistics
- Export stats to CSV
- Custom key groupings
- Per-application key tracking
- Heatmap visualization

## License

MIT License - feel free to use and modify.

---

Built with ❤️ for Windows 11
