# Go for It Qt (`go-for-it-qt` & `go-for-it-tui`)

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Qt 6](https://img.shields.io/badge/Qt-6.5%2B-green.svg)](https://www.qt.io/)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![CI / Release](https://github.com/bladeroot/go-for-it-qt/actions/workflows/release.yml/badge.svg)](https://github.com/bladeroot/go-for-it-qt/actions/workflows/release.yml)

**Go for It Qt** is a high-performance **C++20 / Qt 6** productivity suite combining smart task management, a built-in Pomodoro timer, local **SQLite** storage, dual interfaces (rich **Qt Quick GUI** and standalone **Vim-style Terminal TUI**), and deep synchronization with 7 issue tracking and task management platforms.

---

## ✨ Key Features

### 🖥️ Dual Interfaces (GUI & TUI)
- **Modern Qt Quick GUI (`go-for-it-qt`)**: Sleek desktop application with system-native themes (Fusion/Dark/Light), keyboard navigation, task cards, and quick actions.
- **Standalone Terminal UI (`go-for-it-tui`)**: Pure ANSI terminal interface with full Vim keybindings. Runs seamlessly over SSH, tmux, or headless environments without X11/Wayland dependencies.

### 🌐 7 Native Issue Tracker & Task Integrations
Sync tasks, fetch rich descriptions and comments, and automatically log Pomodoro work sessions back to remote services:
- **GitHub**: Issues, Pull Requests, labels, discussion threads.
- **GitLab**: Issues, Merge Requests, group/project queries, `/spend` time tracking.
- **Atlassian Jira (Cloud & Data Center)**: JQL query support, v3 ADF rich text parsing, automatic worklog logging.
- **JetBrains YouTrack**: Custom queries, markdown descriptions, WorkItem time tracking.
- **Redmine**: Project/query filters, comments, spent time entries.
- **Todoist**: Sync v9 REST API, section/label filtering, instant task updates.
- **Nextcloud Tasks**: CalDAV (`VTODO`) and Nextcloud OCS API for self-hosted cloud privacy.
- **Multi-Account & Multi-List Views**: Aggregate tasks from multiple accounts and providers in single unified views.

### 🎛️ Per-Integration Status & Priority Mapping
- **5-State Local Workflow**: `To-Do`, `In Progress`, `Paused`, `Done`, and `Canceled / Rejected`.
- **Custom Mapping Rules**: Each provider has its own customizable status and priority mapping tables stored in SQLite.
- **ANSI Color Coding in TUI**:
  - `(A)`: **Bold Red** (Critical / High)
  - `(B)`: **Bold Yellow** (Urgent / Medium-High)
  - `(C)`: **Cyan** (Normal)
  - `(D)`: **Green** (Low)
  - `(E)`: **Magenta** (Trivial / Someday)

### 📊 Productivity Dashboard & Billing Reports
- **Interactive Analytics**: Daily, weekly, and monthly Pomodoro completion charts.
- **Context & Project Breakdown**: Track time investment across `+Project` tags and `@Context` tags.
- **Activity Heatmap**: GitHub-style green contribution grid showing daily focus consistency.
- **Export for Billing**: Generate clean **PDF** or **CSV** reports detailing hours spent per task/project for clients, employers, or personal records.

### 🪟 "Always-on-Top" Mini-Player (Picture-in-Picture)
- Minimalist floating semi-transparent widget pinned on top of all windows (`Ctrl+M`).
- Displays the current task, circular countdown timer, and Pause/Done buttons. Perfect for full-screen IDE coding.

### 📅 Due Dates & Smart Lists
- Full standard support for Todo.txt `due:YYYY-MM-DD` tags.
- Smart list views: **Today**, **Overdue**, and **This Week**.
- Proactive desktop notifications when tasks approach deadlines.

### 🎧 Ambient Focus Sound Generator
- Procedural real-time sound synthesizer using `Qt6::Multimedia` / `QAudioSink` (no external audio files needed).
- Concentration soundscapes: **White Noise**, **Gentle Rain**, and **Mechanical Clock Ticking**.
- Automatically mutes during breaks and resumes on work intervals.

### 🪝 Focus Mode System Hooks & DND Integration
- **Deep Focus Mode**:
  - Automatic **Do Not Disturb (DND)**: Inhibit notifications across GNOME Shell, KDE Plasma, and Freedesktop desktops during Work intervals.
  - **Screen Wake / Sleep Inhibit**: Keep screen awake and prevent lock/screensaver during active Pomodoro work.
- **Custom Event Hooks**:
  - Execute arbitrary shell commands or scripts on timer and task events:
    - `on_work_start`: Triggered when work session starts (e.g. update Slack/Discord status, dim smart lights).
    - `on_break_start`: Triggered when break begins (e.g. restore status, notify smart home).
    - `on_pomodoro_completed`: Triggered on successful Pomodoro completion.
    - `on_task_completed`: Triggered when any task is marked Done.
  - Scripts folder: `~/.config/go-for-it-qt/hooks/` with automated template generation.
  - Contextual environment variables: `$GOFI_EVENT`, `$GOFI_TASK_TITLE`, `$GOFI_TASK_KEY`, `$GOFI_TASK_ID`, `$GOFI_PHASE`, `$GOFI_DURATION_SEC`.

### 🚀 Quick Browser Jump ("Open in Browser")
- Open any remote task or PR in your default web browser (`xdg-open`):
  - In TUI: press `w` or `gx`.
  - In GUI: click the 🌐 link button on the task card or details dialog.

### 🗄️ Single-File SQLite Engine (`goforit.db`)
- Complete persistence in a single ACID-compliant database.
- Runtime database path selection (`--db <path>`), one-click **Move Database**, **Export Backup**, and **Import Backup**.

---

## ⌨️ Console Terminal UI (`go-for-it-tui`) Cheat Sheet

Run `go-for-it-tui` or `go-for-it-qt --tui`:

| Key | Mode | Description |
|---|---|---|
| `j` / `k` | Normal | Navigate cursor down / up |
| `gg` / `G` | Normal | Jump to top / bottom of task list |
| `Ctrl+d` / `Ctrl+u` | Normal | Scroll half-page down / up |
| `J` / `K` (`Shift+j/k`) | Normal | Move selected task down / up in order |
| `h` / `l` or `gt` / `gT` | Normal | Switch workflow tab (All, To-Do, In Progress, Paused, Done, Canceled) |
| `1` .. `6` | Normal | Direct jump to tab 1 through 6 |
| `Space` or `x` | Normal | Toggle task status between Done and To-Do |
| `s` | Normal | Open 5-state status picker |
| `P` | Normal | Cycle task priority `(A)` -> `(B)` -> `(C)` -> `(D)` -> `(E)` -> None |
| `Enter` | Normal | Select task as active Pomodoro task |
| `t` | Normal | Start / Pause Pomodoro timer |
| `B` | Normal | Skip current Pomodoro phase |
| `R` | Normal | Reset current Pomodoro timer phase |
| `w` or `gx` | Normal | **Open task / PR in web browser** (`xdg-open`) |
| `o` / `a` | Normal | Add new task (enter `INSERT` mode) |
| `i` / `e` / `cw` | Normal | Inline edit selected task title |
| `E` (`Shift+e`) | Normal | Open multi-line description and comments editor |
| `dd` | Normal | Delete selected task |
| `r` / `Ctrl+r` | Normal | Sync current list with remote provider |
| `L` / `[` / `]` | Normal | Open list picker (`L`) / switch previous (`[`) / next (`]`) list |
| `p` | Normal | Toggle live preview pane |
| `Ctrl+e` / `Ctrl+y` | Normal | Scroll preview pane down / up |
| `/` | Normal | Live filter / search tasks (`Esc` to exit, `:noh` to clear) |
| `?` or `:help` | Normal | Show interactive keyboard shortcuts help screen |
| `:p <A-E>`, `:priority` | Ex | Set priority directly (e.g. `:p A` or `:p none`) |
| `:dnd [on/off]` | Ex | Toggle Do Not Disturb (DND) focus mode |
| `:hooks [on/off]` | Ex | Enable / disable user event hooks |
| `:hook test <event>` | Ex | Test execute hook (`work_start`, `break_start`, `task_completed`) |
| `:sync` | Ex | Synchronize active list with remote tracker |
| `:sort <mode>` | Ex | Sort tasks by `priority`, `date`, `due`, or `title` |
| `:clear` | Ex | Clear completed / canceled tasks |
| `:list <id>` | Ex | Switch list by ID |
| `:q` / `:wq` | Ex | Quit TUI |

---

## 🛠️ Build & Installation

### Dependencies
- **CMake** `>= 3.21`
- **C++20 compiler** (GCC `>= 11` or Clang `>= 14`)
- **Qt 6** (`>= 6.5`):
  `qt6-base`, `qt6-declarative`, `qt6-multimedia`, `libqt6sql6-sqlite`

#### Ubuntu 24.10+ / Debian 13+ (Trixie/Sid):
```bash
sudo apt update
sudo apt install -y cmake ninja-build g++ qt6-base-dev qt6-declarative-dev \
    qt6-multimedia-dev libqt6sql6-sqlite qml6-module-qtquick \
    qml6-module-qtquick-controls qml6-module-qtquick-layouts \
    qml6-module-qtquick-dialogs qml6-module-qtquick-window
```

> [!NOTE]
> Older Ubuntu releases (22.04 LTS / 24.04 LTS) ship with Qt 6.2 / 6.4 in default repositories. To build from source on 22.04 / 24.04, install Qt `>= 6.5` using [aqtinstall](https://github.com/miurahr/aqtinstall) or the official Qt online installer.

#### Arch Linux / Manjaro:
```bash
sudo pacman -S cmake ninja gcc qt6-base qt6-declarative qt6-multimedia
```

### Compile & Run Tests
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

### Install System-Wide
```bash
sudo cmake --install build --prefix /usr/local
```

---

## 📦 Packages

### Debian / Ubuntu (`.deb`)
Download pre-built `.deb` binaries from the [GitHub Releases](https://github.com/bladeroot/go-for-it-qt/releases) page, or build locally:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build build -j$(nproc)
cpack --config build/CPackConfig.cmake -G DEB -B build
sudo dpkg -i build/go-for-it-qt-*.deb
```

### Arch Linux (`PKGBUILD`)
Use the included `PKGBUILD`:
```bash
makepkg -si
```

---

## 📜 Credits & License

- **Go for It Qt (`go-for-it-qt`)** — C++20 / Qt 6 rewrite, Terminal TUI, multi-tracker integrations, analytics, and audio synthesizer by **Yurii Myronchuk ([@bladeroot](https://github.com/bladeroot))**.
- Inspired by the classic **Go-For-It!** (Vala / GTK3) by **Manuel Kehl** and **Jonathan Moerman**.
- Licensed under the **GNU General Public License v3.0 (GPL-3.0)** — see [LICENSE](LICENSE).
