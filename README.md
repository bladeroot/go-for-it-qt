# Go for It Qt (`go-for-it-qt`)

**Go for It Qt** is a modern **C++20 / Qt 6 (Qt Quick / QML)** rewrite and evolution of the classic [Go-For-It!](https://github.com/JMoerman/Go-For-It) productivity app — combining task management with a built-in Pomodoro timer, local **SQLite** storage, and native **Jira** & **JetBrains YouTrack** integration.

---

## ✨ Key Features

- **Modern C++20 & Qt 6 (Qt Quick Controls 2)** architecture with dark/light theme support.
- **Single-File SQLite Storage (`goforit.db`)**:
  - Stores all task lists, tasks, descriptions, comments, Pomodoro activity logs, and app settings in a single portable SQLite database.
  - Configurable database location (`Settings -> Storage` or `--db <path>`) with one-click **Move Database**, **Export Backup**, and **Import Backup**.
- **Jira (Cloud / Data Center) & JetBrains YouTrack Integration**:
  - Sync issues via custom **JQL** (Jira) or **YouTrack search queries**.
  - Downloads full issue descriptions (including Jira v3 ADF rich text) and comments.
  - **Automatic Pomodoro Worklog / WorkItem logging**: completed work intervals are automatically logged back to the active Jira/YouTrack issue.
- **Extended 5-State Task Workflow & Remote Status Mapping**:
  - Local statuses: **To-Do**, **In Progress**, **Paused**, **Done**, and **Canceled / Rejected**.
  - Fetch remote statuses directly from Jira/YouTrack (`Load Statuses`) and map each remote status to a local status view.
- **Pomodoro & Custom Timer Schedules**:
  - Configurable work/break schedules (classic Pomodoro or multi-stage custom schedules), per-list timer overrides, sound notifications (`Qt6::Multimedia`), Freedesktop DBus notifications, System Tray AppIndicator, and Unity LauncherEntry dock progress/badge.


---

## ⌨️ Console Terminal UI (`go-for-it-tui`) with Vim Keybindings

Run the standalone terminal UI binary `go-for-it-tui` (works over SSH / without X11 or Wayland) or pass `--tui` (`-t`) to `go-for-it-qt`:
```bash
go-for-it-tui
# or
go-for-it-qt --tui
```

### Vim Keybindings
| Key | Action |
|---|---|
| `j` / `k` / `gg` / `G` / `Ctrl+d` / `Ctrl+u` | Move cursor down / up / top / bottom / half-page |
| `J` / `K` (`Shift+j` / `Shift+k`) | Move selected task down / up in order |
| `h` / `l` or `1`..`6` or `gt` / `gT` | Switch status tab (`All Open`, `To-Do`, `In Progress`, `Paused`, `Done`, `Canceled`) |
| `Space` or `x` | Toggle task `Done` / `To-Do` |
| `s` | Open status picker (`1:To-Do`, `2:In Progress`, `3:Paused`, `4:Done`, `5:Canceled`) |
| `o` / `a` | Add new task (`INSERT` mode) |
| `i` / `e` / `cw` | Edit selected task title inline |
| `E` (`Shift+e`) | Edit selected task full description |
| `dd` | Delete selected task |
| `Enter` | Select task as active Pomodoro task |
| `t` / `S` / `B` / `R` | Start/Pause Pomodoro timer (`t`), Skip phase (`B`), Reset phase (`R`) |
| `r` / `Ctrl+r` | Sync current list with Jira / YouTrack |
| `L` or `[` / `]` | Open List Picker (`L`) or switch to previous/next list (`[` / `]`) |
| `p` / `Ctrl+e` / `Ctrl+y` | Toggle preview pane (`p`), scroll description & comments (`Ctrl+e` / `Ctrl+y`) |
| `/` | Live search/filter tasks (`Esc` or `:noh` to clear) |
| `:sync`, `:sort`, `:clear`, `:list`, `:q` | Vim Ex commands |
| `?` or `:help` | Toggle keyboard shortcuts help screen |

---

## 🛠️ Build & Run

### Dependencies (Arch / Ubuntu / Fedora)
- CMake `>= 3.21`
- C++20 compiler (`GCC >= 11` or `Clang >= 14`)
- Qt 6 (`>= 6.5`): `Core`, `Gui`, `Widgets`, `Qml`, `Quick`, `QuickControls2`, `Multimedia`, `DBus`, `Sql` (`QSQLITE`), `Network`, `Test`

### Compile & Test
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Install
```bash
sudo cmake --install build --prefix /usr/local
```

---

## 📜 Credits & License

- **Go for It Qt (`go-for-it-qt`)** — C++20 / Qt 6 port, SQLite storage engine, extended statuses, and Jira/YouTrack integration by **Yurii Myronchuk (@bladeroot)**.
- Based on the original **Go-For-It!** (Vala / GTK3) created by **Manuel Kehl** (2014–2017) and **Jonathan Moerman** (2017–2021).
- Licensed under the **GNU General Public License v3.0 (GPL-3.0)** — see [LICENSE](LICENSE).

---

## 📦 System Packaging

### Arch Linux / CachyOS (`PKGBUILD`)
A ready-to-use `PKGBUILD` is included in the repository root and in `packaging/arch/PKGBUILD`:
```bash
makepkg -si
```

### Debian / Ubuntu (`.deb`)
You can build a `.deb` package directly with **CPack**:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build build -j$(nproc)
cpack --config build/CPackConfig.cmake -G DEB -B build
```
Or using standard `dpkg-buildpackage` with `packaging/debian`:
```bash
ln -sf packaging/debian debian
dpkg-buildpackage -us -uc -b
```
