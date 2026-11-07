# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [2.0.0] - 2026-11-07

### 🚀 Major Architecture Rewrite (C++20 & Qt 6)
- Complete rewrite from original Vala/GTK3 code to modern **C++20** and **Qt 6** (`Qt Quick Controls 2` / `QML` and standalone terminal client).
- **Single-File SQLite Engine (`goforit.db`)**:
  - Replaced text-file synchronization issues with a transactional SQLite database engine.
  - Full support for live migrations, runtime database relocation (`--db <path>`), and instant backup import/export.
- High-performance, low-latency architecture with zero third-party framework dependencies outside Qt 6.

### 🌐 7 Native Issue Tracker & Task Integrations
- **GitHub**: Synchronize repository Issues and Pull Requests; filter by repo/label; download discussion threads and PR metadata; auto-log time.
- **GitLab**: Synchronize group and project Issues and Merge Requests; fetch full discussions; native spent time logging (`/spend` API).
- **Jira (Cloud & Data Center)**: Synchronize issues via custom JQL filters; parse Jira v3 ADF rich text and descriptions; automatic Pomodoro worklog logging.
- **JetBrains YouTrack**: Sync issues via custom YouTrack search queries; markdown rendering; automatic WorkItem time tracking.
- **Redmine**: REST API sync with custom project/query filters; spent time logging via Redmine Time Entries API.
- **Todoist**: Sync v9 REST API integration with section and label mapping; real-time task updates.
- **Nextcloud Tasks**: CalDAV (`VTODO`) and Nextcloud OCS API sync for 100% private self-hosted cloud task management.
- **Unified Multi-Account & Multi-List Views**:
  - Aggregate tasks across multiple accounts and providers in unified views.

### 🎯 Remote Status & Priority Mapping
- **Local 5-State Workflow**: To-Do, In Progress, Paused, Done, and Canceled / Rejected.
- **Per-Integration Custom Mapping**:
  - Flexible per-provider rules to map any remote status to local workflow states.
  - Remote priority mapping: assign remote priorities (e.g. Critical, Blocker, High, Low, P1..P5) to local Todo.txt priorities `(A)`, `(B)`, `(C)`, `(D)`, `(E)` or `None`.
  - Configurable via dedicated GUI settings tabs and persisted in SQLite.

### ⌨️ Standalone Terminal UI (`go-for-it-tui`) with Vim Keys
- Standalone ncurses-free ANSI terminal UI executable (`go-for-it-tui`) or via `go-for-it-qt --tui`.
- **Full Vim Navigation**:
  - Motion: `j` / `k` / `gg` / `G` / `Ctrl+d` / `Ctrl+u`, item reordering (`Shift+j` / `Shift+k`).
  - Modes: Normal, Insert (`o`, `a`), Inline Edit (`i`, `e`, `cw`), Multi-line description editor (`Shift+e`).
  - Priority cycling (`P`) and Ex-commands (`:p <A-E>`, `:priority <A-E>`).
  - Quick browser jump (`w` or `gx`) to open Jira/GitHub/GitLab/YouTrack/Redmine/Todoist issue URL in default browser.
  - Live preview pane (`p`), live search (`/`), list picker (`L`), status switcher (`1`..`6` or `gt`/`gT`).
- **ANSI Color Coding**:
  - Vivid visual distinction for priorities: `(A)` Bold Red, `(B)` Bold Yellow, `(C)` Cyan, `(D)` Green, `(E)` Magenta.
  - Rendered across task lists, preview pane, and standalone markdown viewer (`--md <file>`).

### 📊 Productivity Dashboard & Billing Reports
- Interactive visual statistics for completed Pomodoro intervals:
  - Daily, weekly, and monthly productivity breakdowns.
  - Time allocation distribution by project (`+Project`) and context (`@Context`).
- **GitHub-style Heatmap**: Visual activity grid displaying intensity of completed focus sessions over time.
- **Billing & Freelance Export**:
  - Export professional PDF and CSV reports detailing hours spent per task and project for clients or management.

### 🪟 "Always-on-Top" Mini-Player (Picture-in-Picture Widget)
- Compact, floating, semi-transparent window pinned above all workspaces (`Ctrl+M` toggle).
- Displays the active task title, countdown ring, and one-click Pause/Done controls.
- Optimized for distraction-free coding inside full-screen IDEs or multi-monitor setups.

### 📅 Due Dates & Smart Lists
- Full standard support for Todo.txt `due:YYYY-MM-DD` tags.
- Built-in smart lists: **Today**, **Overdue**, and **This Week**.
- Proactive desktop notifications alerting when deadlines approach.

### 🎧 Ambient Focus Sound Generator
- Procedural real-time sound synthesizer powered by `Qt6::Multimedia` / `QAudioSink`.
- Background soundscapes to enhance concentration: **White Noise**, **Gentle Rain**, and **Mechanical Clock Ticking**.
- Automatic smart pause during Pomodoro break intervals and resume during work cycles.

### 📦 Packaging & CI/CD
- GitHub Actions CI workflow generating Ubuntu/Debian `.deb` packages automatically on tagged releases (`v*`).
- Ready-to-build Arch Linux `PKGBUILD` and Debian packaging files.

---

## [1.9.5] - 2026-11-06

### Added
- Initial C++20 / Qt 6 port of Go-For-It with SQLite storage engine.
- Jira and JetBrains YouTrack initial synchronization.
- Basic Pomodoro timer and Qt Quick Controls 2 user interface.
