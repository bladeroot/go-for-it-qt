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
