# Copilot instructions

## Build and test

Build outside the repository so generated files do not pollute the worktree:

```bash
build_dir=$(mktemp -d /tmp/go-for-it-build.XXXXXX)
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build "$build_dir" -j2
QT_QPA_PLATFORM=offscreen ctest --test-dir "$build_dir" --output-on-failure
```

All Qt tests are slots in `tst_todotxt`. Run one slot with
`QT_QPA_PLATFORM=offscreen "$build_dir/tst_todotxt" testDueDatesAndSmartFilters`;
list slots with `"$build_dir/tst_todotxt" -functions`. Remove the temporary build
directory when finished. There is no configured linter or formatter.

## Architecture

- `gofi_core` is the shared static library for `go-for-it-qt`, `go-for-it-tui`,
  and `tst_todotxt`. `AppController` is the composition root: QML receives it as
  the `app` context object, and the TUI uses the same controller. Put behavior
  shared by both interfaces in the controller or its services, not in QML or
  `TuiApp`.
- Persistence is SQLite through the process-wide `DatabaseManager` singleton.
  `TxtTask` handles Todo.txt line parsing/serialization; it is not the storage
  layer. When changing the schema, update both the table definition and its
  matching `ALTER TABLE` in `DatabaseManager::createSchema()`.
- `TaskManager` owns todo and done source models; `AppController` exposes proxy
  models to QML. Controller invokables that take a row receive a **proxy row**,
  so map it to the source model before operating on a task.
- Remote provider fetching is paginated and collected before
  `IssueTrackerService::processSyncedItems()` writes and reconciles results.
  Reconciliation can mark missing open tasks done: only allow it after a
  complete, error-free response (including a correctly identified empty
  response), and keep the sync's database writes transactional. Check
  `knownbug.md` before changing sync, timer, or translation behavior.

## Repository-specific conventions

- Register every new C++ source/header in `GOFI_CORE_SOURCES` in
  `CMakeLists.txt`. Register new QML files in `qt_add_qml_module`'s `QML_FILES`.
  If a QML import changes, update both CPack dependencies in `CMakeLists.txt`
  and `debian/control`.
- Tests run in a `QTemporaryDir` sandbox. Initialize the database explicitly
  for each test; do not let test code fall back to the user's database. Keep
  desktop integrations, audio, hooks, and remote writes disabled in tests;
  offscreen Qt alone does not disable these side effects. Use a fixed reference
  date for due-date tests and a local `QTcpServer` for tracker HTTP tests.
- Translation dictionaries are flat JSON files in `data/translations/`; keys
  are exact English source strings. Keep the language files' key sets aligned.
  Build-tree data is copied at CMake configure time, so reconfigure after
  editing translations. Preserve the resource initialization in
  `UkTranslator.cpp` when changing embedded translations.
- Use a throwaway `--db` path for manual runs that may write data. User
  preferences are split between `go-for-it.ini`, `shortcuts.ini`, and
  `settings.ini`; do not treat legacy `lists.ini` as active configuration.
- Follow the nearby C++/QML style and Conventional Commit format used in the
  repository (for example, `fix(qml): handle missing property`).
