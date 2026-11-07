# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Repository layout, coding style, test naming and commit conventions are in AGENTS.md:

@AGENTS.md

## Commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure

# Run a single Qt Test slot (all tests live in one binary, tst_todotxt)
QT_QPA_PLATFORM=offscreen build/tst_todotxt testDueDatesAndSmartFilters
QT_QPA_PLATFORM=offscreen build/tst_todotxt -functions   # list slots

# .deb package (README recipe; CI in .github/workflows/release.yml uses the same flags but keeps tests on and runs ctest first)
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build build && (cd build && cpack -G DEB)
```

Both binaries accept `--db <file>` (use a throwaway SQLite file while experimenting instead of the user's real DB), `--load <list-id>`, `--list`, `--logfile`. `go-for-it-qt --tui` also launches the TUI; `go-for-it-tui --md <file>` opens the Markdown viewer. No linter or formatter is configured.

## Architecture

- **Everything except `main*.cpp` is compiled into one static library, `gofi_core`** (including `src/tui/TuiApp.cpp`), which is linked by `go-for-it-qt`, `go-for-it-tui`, and `tst_todotxt`. New source files must be added by hand to `GOFI_CORE_SOURCES` in `CMakeLists.txt`; new QML files must be added to the `qt_add_qml_module` `QML_FILES` list (QML module URI `GoForIt`, loaded via `engine.loadFromModule("GoForIt", "Main")`).
- **`gofi::AppController` is the single composition root.** It owns all services as value members (settings, timer, list manager, `TaskManager`, `IssueTrackerService`, analytics, ambient sound, system hooks, desktop integration, etc.) and does the wiring between them. The GUI exposes it to QML as the context property `app` (`app.timer`, `app.listManager`, `app.todoProxyModel`, …); the TUI drives the same `AppController` directly. Behaviour shared by both front ends belongs in `AppController`/services, not in QML or `TuiApp`.
- **Storage is SQLite, not todo.txt files** (AGENTS.md's "Todo.txt parsing and storage" is outdated on this point). `DatabaseManager` is a process-wide singleton: `AppController`'s constructor calls `DatabaseManager::instance().initialize(customDbPath)`, and an empty path falls back to `Storage/databasePath` from `settings.ini`, else `<AppDataLocation>/goforit.db` (`~/.local/share/go-for-it-qt/go-for-it-qt/goforit.db`). Tables: `lists` (incl. provider credentials and JSON `status_mapping` / `priority_mapping`), `tasks`, `activity_log`, `app_settings`. Schema migrations are done in `createSchema()` by unconditional `ALTER TABLE … ADD COLUMN` statements whose failure (column already exists) is ignored: to add a column, add it to the `CREATE TABLE` *and* append a matching `ALTER TABLE`. `src/todotxt/` keeps only the Todo.txt *line format* (`TxtTask` parses/serializes priority, `+project`, `@context`, `due:`, `duration:` etc.).
- **Config is spread over several INI files** in `~/.config/go-for-it-qt/` (`QSettings` org `go-for-it-qt`): `go-for-it.ini` (`SettingsManager`: timer, theme, language, hooks…), `shortcuts.ini` (`ShortcutManager`), `settings.ini` (only the DB location). `lists.ini` is a legacy file that the code no longer reads. User hook scripts live in `~/.config/go-for-it-qt/hooks/` (`SystemHookService::hooksDirectory()`; the legacy `~/.config/go-for-it/hooks/` is no longer read). `settings.ini` also holds `State/gnome_banners_inhibited[_pid]`, used to restore GNOME banners after a crash during a DND work session.
- **Task models:** `TaskManager` holds two `TaskStoreModel`s (todo / done) for the active list or a multi-list selection (`__ALL__` or `*` = all lists), moves tasks between them on done-toggle, and writes changes back to the DB. `AppController` wraps each in a `TaskFilterProxyModel`; QML and `AppController` invokables address tasks by `(fromDoneList, proxyRow)`, so proxy→source row mapping matters when touching those methods.
- **Remote trackers:** `IssueTrackerService` (largest file) implements GitHub, GitLab, Jira, YouTrack, Redmine, Todoist and Nextcloud (CalDAV) with `if (provider == "…")` branches keyed by the list's `provider` string. Fetched issues land in the local DB via `DatabaseManager::upsertRemoteIssueTask`, with remote status/priority translated through the list's mappings (local statuses: `todo`, `in_progress`, `paused`, `done`, `canceled`; priorities `A`–`E` or `none`).
- **Translations** are not `.ts`/`.qm` files: `UkTranslator` is a `QTranslator` subclass with an in-code English→Ukrainian dictionary. When adding user-visible strings in QML/C++, add the Ukrainian entry there; language switches trigger `QQmlApplicationEngine::retranslate`.

## Testing notes

- `initTestCase()` sandboxes the whole run in the `m_testSandbox` `QTemporaryDir`: it points `HOME`, `XDG_CONFIG_HOME` and `XDG_DATA_HOME` there, calls `QStandardPaths::setTestModeEnabled(true)`, and verifies that `QSettings` resolves inside the sandbox. This only works because no `QSettings`/`QStandardPaths` lookup happens before it, so don't add such code to the test-class constructor or to static initializers. Put test files under `m_testSandbox.filePath(...)`, not in fixed `/tmp` paths.
- `DatabaseManager` is a singleton. `cleanup()` calls `DatabaseManager::instance().close()` after every test, so each test must open its own DB with `AppController(logPath, dbPath)` or `DatabaseManager::instance().initialize(path)` (`":memory:"` works). A bare `AppController controller;`, or `ListManagerModel` finding the DB closed, falls back to the default DB path. Inside the sandbox that is harmless, outside it it means the user's real DB.
- Real desktop side effects are switched off in tests by two mechanisms:
  - Explicit flags set in `initTestCase()`: `NotificationService::setAudioPlaybackEnabled(false)` disables audio, and `SystemHookService::setSystemIntegrationEnabled(false)` makes hook processes emit `[Simulated in test mode]` instead of running.
  - `QStandardPaths::isTestModeEnabled()` checks inside production code: audio, DND, sleep inhibit, Freedesktop `Notify`, the Unity launcher badge, the due-reminder timer, and hooks-dir / GNOME crash-recovery logic.
  - Any new code that touches D-Bus, `gsettings`, processes or audio needs the same guard. `QT_QPA_PLATFORM=offscreen` no longer disables anything; `GOFI_NO_SOUND` still disables audio.
- `testRealHookProcessExecutionInSandbox` temporarily re-enables `setSystemIntegrationEnabled(true)` to run a real `/bin/sh` hook inside the sandbox.
- Due-date logic takes a reference date (`TxtTask::isOverdue/isDueToday/isDueThisWeek(refDate)`, `TaskFilterProxyModel::setReferenceDate`). Tests use a fixed date instead of `QDate::currentDate()`.

## CI

Minimum Qt is 6.5 (`find_package(Qt6 6.5 ...)`); Qt < 6.5 is not supported.

`.github/workflows/release.yml` runs on pushes to `main`, PRs and `v*` tags, inside a `debian:trixie-slim` container with distro Qt (6.8). It builds, runs ctest, then `cpack -G "DEB;TGZ"` with `CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON`, and publishes to GitHub Releases only on tag pushes.

There are two independent .deb definitions, and both must be kept in sync when QML imports change:
- CPack: `CPACK_DEBIAN_PACKAGE_DEPENDS` in `CMakeLists.txt`.
- Native Debian packaging: the `Depends` field in `debian/control`.

`dpkg-shlibdeps` only detects linked C++ libraries, so every `import QtQuick.*` module in `qml/` needs its `qml6-module-*` package listed explicitly. Avoid `| libqt6quick6` alternatives: that library is always pulled in by shlibs, so the alternative silently drops the QML module. The resulting `.deb` requires Qt ≥ 6.8 (Debian 13, Ubuntu 25.04+). To verify a package, install it into a clean `debian:trixie-slim` container and run `QT_QPA_PLATFORM=offscreen go-for-it-qt`.
