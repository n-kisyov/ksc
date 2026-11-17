# ⌨️ ksc — Keystroke Counter

**v1.1** — A lightweight Windows desktop app in modern C that tracks every keystroke, maps keyboard heatmaps, simulates input, records to a keylogger, and syncs encrypted backups to Google Drive and SSH with Telegram notifications.

<p align="center">
  <img src="screenshots/main-window-dark.png" width="400" alt="Main Window Dark">
  <img src="screenshots/main-window-light.png" width="400" alt="Main Window Light">
</p>

---

## 📸 Screenshots

| 🌙 Dark Mode | |
|---|---|
| ![Heatmap](screenshots/keyboard-heatmap-dark.png) | ![Keyboard Sim](screenshots/keyboard-sim-dark.png) |
| ![Mouse Clicker](screenshots/mouse-clicker-dark.png) | ![Stats](screenshots/stats-window-dark.png) |

---

## ✨ Features

### ⌨️ Core Tracking
- **Keystroke & mouse counting** — Low-level hooks (`WH_KEYBOARD_LL` + `WH_MOUSE_LL`) capture every keypress and left/middle/right/X1/X2 mouse clicks system‑wide (excluding app‑injected simulator/clicker input), with a **Pause Tracking** toggle in the File & tray menus
- **Per‑app tracking** — Records foreground process executable name (e.g. `chrome.exe`, `code.exe`) with full window title preserved in the keylogger
- **Live totals & typing speed** — 1‑second status bar and tray tooltip updates showing total keyboard/mouse counts, Today's count, and rolling live WPM / session Peak WPM
- **Lock‑free batch writes** — Ring buffer copied out under critical section so hook callbacks never block on SQLite `BEGIN/COMMIT` disk flushes

### 🪟 Windows & Views
- **Keyboard heatmap** — Full QWERTY + numpad + arrow keys + 5 mouse buttons (`LMB`, `MMB`, `RMB`, `M4`, `M5`) colored by usage frequency (blue → red gradient), hover readout panel, per‑app and time‑period filters (`All Time`, `Today`, `Last 7 Days`, `Last 30 Days`), double‑buffered rendering
- **Statistics** — Custom date range pickers, app filter, **By Key / By Application / By Day** view modes, keyboard/mouse separated totals, sortable columns with header sort arrows, 10s auto‑refresh preserving scroll position, properly escaped CSV export
- **Keylogger** — Timestamped per‑key recording to a separate `ksc_keylog.db`; dedicated viewer window with date, app, and live keyword search filters, sortable columns, entry counter, and CSV export
- **Mouse clicker** — Configurable click interval (min/sec/ms), random offset, Left/Right/Middle button, single or double click, Cursor or Fixed `(X, Y)` coordinates with 3‑second cursor picker, continuous/limited mode, system‑wide start/stop hotkeys
- **Keyboard simulator** — Record up to 128 key combo sequences, replay with interval + random offset (including extended keys), system‑wide start/stop hotkeys

### ☁️ Cloud Backup
- **Google Drive** — OAuth2 with PKCE login via browser, `ksc-backups` folder auto‑created in Drive root, DPAPI‑encrypted token storage
- **SSH / SFTP** — libssh2 with OpenSSL backend, uploads to `~/ksc-backups`, password encrypted with DPAPI
- **Switchable ZIP compression** — Optional checkbox to compress each database into a separate `.zip` archive before uploading
- **Retention limit** — Auto‑prune older backups on Drive and SSH (`All`, `10`, `30`, or `50` backups)
- **Scheduler** — Off / 5 min / 15 min / 30 min / 1 h / 12 h / daily
- **Multi‑target** — Backups go to Drive, SSH, or both; local files deleted only after ALL targets succeed
- **Safe online backup** — Uses `sqlite3_backup_init()` API (WAL‑safe) for both main and keylogger databases
- **WAL mode** — SQLite journal mode enabled for concurrent reads during writes

### 🔔 Notifications
- **Telegram** — Bot token + group chat ID; sends status after each sync (success + failures, or failures only)
- **Test message** button in Settings for end‑to‑end verification

### 🎨 UI
- **Dark mode** — Full dark theme: title bar, scrollbar, menus, list views, all child windows via `DwmSetWindowAttribute` + `AllowDarkModeForWindow`
- **System tray** — Minimize to tray on close; today's count in tooltip; right‑click for quick access
- **Show/hide hotkey** — Configurable `Ctrl+Shift+K` to raise the window from anywhere
- **Start minimized** — Launch directly to system tray
- **Start with Windows** — Registry Run key toggle

### 🛠️ Data Management
- **Safe backup & restore** — Online backup via Save dialog (`.zip` or `.db` for both main and companion keylogger DB); one‑click restore from `.db` or `.zip`
- **Portable mode** — Pass `--portable` or place an empty `portable.txt` next to `ksc.exe` to store databases alongside the executable instead of `%APPDATA%\KSC`
- **CSV export** — All‑time, date‑filtered, or keylogger search‑filtered CSV export
- **Reset statistics** — Wipe counts, keep settings
- **Single instance** — Named mutex prevents duplicate processes and focuses the running window

### 🔐 Build & Distribution
- **Static linking** — Standalone `.exe` (zero DLL dependencies)
- **Self‑signed Authenticode** — Code‑signed with timestamp at build time; one‑time trust command eliminates SmartScreen
- **VERSIONINFO** — File properties show version `1.1`, company `bbounce.org`
- **Hand‑written C23** — No frameworks, no external UI libs

---

## ⚙️ Settings

| Setting | Default | Description |
|---|---|---|
| Start with Windows | OFF | Launches ksc on login |
| Start minimized to tray | OFF | Starts hidden |
| Dark mode | OFF | Full dark theme |
| Auto‑refresh stats | ON | Every 10 seconds |
| Enable Keylogger | OFF | Records to `ksc_keylog.db` |
| Reset All Statistics | — | Clears counts, keeps settings |
| Delete Keylogger Logs | — | Deletes keylogger database (disabled while keylogger on) |
| Show KSC Shortcut | `Ctrl+Shift+K` | Configurable system‑wide |
| Telegram Bot Token | — | From @BotFather |
| Group Chat ID | — | Telegram group ID |
| Enable Telegram | OFF | Sends sync status |
| Notify mode | All | Success + failures, or failures only |

All settings stored in SQLite.

---

## 🚀 Quick Start

```powershell
.\build.ps1
```

Requires **CMake 3.15+**, **GCC (MinGW‑w64)**, and **OpenSSL** (for SSH). The script:
1. Downloads SQLite3 amalgamation + libssh2 source
2. Builds libssh2 (OpenSSL backend)
3. Generates the app icon
4. Compiles everything with static linking
5. Self‑signs the `.exe`

To eliminate SmartScreen warnings (run once as Admin):
```powershell
Import-Certificate -FilePath .\ksc.cer -CertStoreLocation Cert:\CurrentUser\TrustedPublisher
```

---

## 🧱 Tech Stack

| Layer | Choice |
|---|---|
| Language | C23 (`-std=c23`) |
| Compiler | GCC (MinGW‑w64) |
| Build | CMake |
| Database | SQLite3 (amalgamation, WAL mode) |
| HTTP | WinHTTP |
| SSH | libssh2 + OpenSSL |
| Compression | zlib (PKZIP `.zip` archives) |
| Crypto | DPAPI, CryptGenRandom, BCrypt |
| UI | Win32 API |
| Linking | Fully static |

---

## 🗺️ Future Plans

- Idle detection & session tracking
- Linux support (X11/Wayland)

---

## 📄 License

MIT — feel free to use and modify.

---

Built with ❤️ for Windows 11
