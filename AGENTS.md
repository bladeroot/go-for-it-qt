# Repository Guidelines

## Project Structure & Module Organization

This C++20/Qt 6 application has two front ends. Core services, SQLite persistence, and models live in `src/core/`; Todo.txt handling is in `src/todotxt/`; the terminal UI is in `src/tui/`. Entry points are `src/main.cpp` and `src/main_tui.cpp`. QML views are grouped under `qml/components/`, `qml/dialogs/`, and `qml/pages/`. Runtime assets belong in `data/`, tests in `tests/`, and distribution files in `packaging/`, `debian/`, and `PKGBUILD`.

## Build, Test, and Development Commands

Build outside the repository. Create a disposable directory under `/tmp`, then remove it after verification:

```bash
build_dir=$(mktemp -d /tmp/go-for-it-build.XXXXXX)
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build "$build_dir" -j2
ctest --test-dir "$build_dir" --output-on-failure
cmake -E remove_directory "$build_dir"
```

Run `$build_dir/go-for-it-qt` for the GUI or `$build_dir/go-for-it-tui` for the terminal interface before cleanup. Use `-DCMAKE_BUILD_TYPE=Release` for production builds. Never leave generated build artifacts in the working tree. Register new C++ files in `GOFI_CORE_SOURCES` and QML files in the `QML_FILES` list in `CMakeLists.txt`.

## Coding Style & Naming Conventions

Use four-space indentation, same-line braces, `PascalCase` classes, and `camelCase` methods and variables. Pair `ClassName.h` with `ClassName.cpp`, use `QStringLiteral` for fixed Qt strings, and place application types in `gofi`. Name QML components `PascalCase.qml` and properties or IDs in lower camel case. No repository-wide formatter is configured; preserve nearby formatting.

## Testing Guidelines

Tests use Qt Test and are registered through CTest. Add focused private slots to `tests/tst_todotxt.cpp`, naming them `testBehaviorOrFeature`. Exercise model state, persistence, parsing, rollback behavior, and signal side effects with deterministic data. Run one slot as `$build_dir/tst_todotxt testBehaviorOrFeature`. CMake sets the offscreen Qt platform for CTest. Always run the full suite and add regression coverage for bug fixes, including failure paths.

## Configuration & Safety

Manual runs use persistent user settings and SQLite data. Pass `--db /tmp/goforit-test.db` while experimenting to avoid modifying normal data. Keep multi-step SQLite changes transactional, check every database result, and do not trigger notifications or remote updates until local persistence succeeds. Guard audio, D-Bus, process, and desktop integrations in test mode.

## Commit & Pull Request Guidelines

Follow the history's Conventional Commit pattern, such as `feat(tui): add shortcut` or `fix(qml): handle missing property`. Keep each commit scoped to one logical change. Pull requests should explain the user-visible effect, list verification commands, link relevant issues, and include screenshots or recordings for GUI/QML changes. Call out schema, packaging, or dependency changes explicitly.
