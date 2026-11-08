# Repository Guidelines

## Project Structure & Module Organization

This C++20/Qt 6 application has GUI and terminal front ends. Core services and SQLite persistence live in `src/core/`; Todo.txt handling is in `src/todotxt/`; terminal code is in `src/tui/`. QML views are under `qml/`, assets under `data/`, tests in `tests/`, and distribution files in `packaging/` and `debian/`.

## Build, Test, and Development Commands

Build outside the repository. Create a disposable directory under `/tmp`, then remove it after verification:

```bash
build_dir=$(mktemp -d /tmp/go-for-it-build.XXXXXX)
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build "$build_dir" -j2
ctest --test-dir "$build_dir" --output-on-failure
cmake -E remove_directory "$build_dir"
```

Run `$build_dir/go-for-it-qt` or `$build_dir/go-for-it-tui` before cleanup. Never leave generated artifacts in the repository. Register new C++ and QML files in `CMakeLists.txt`.

## Coding Style & Naming Conventions

Use four-space indentation, same-line braces, `PascalCase` classes, and `camelCase` members. Pair `ClassName.h` with `ClassName.cpp` and use `QStringLiteral` for fixed strings. Name QML components `PascalCase.qml` and IDs in lower camel case. No formatter is configured; preserve nearby style.

## Testing Guidelines

Tests use Qt Test through CTest. Add focused private slots to `tests/tst_todotxt.cpp`, named `testBehaviorOrFeature`. Exercise persistence, rollback, model state, parsing, and signal side effects with deterministic data. Run one slot as `$build_dir/tst_todotxt testBehaviorOrFeature`, then always run the full suite.

## Review & Known-Bug Workflow

Review the repository after every commit or relevant working-tree change; do not limit review to the latest diff. For each reproducible defect, add a regression test first and verify it. Only afterward update `knownbug.md`. Keep every unresolved issue under `Open`, repeat still-present findings in subsequent reviews, and move verified fixes to `Resolved`. Record the reviewed commit and test results. During active monitoring, check HEAD, `git status`, file diffs, and `knownbug.md` contents at least once per minute.

## Configuration & Safety

Manual runs use persistent user settings and SQLite data. Pass `--db /tmp/goforit-test.db` while experimenting to avoid modifying normal data. Keep multi-step SQLite changes transactional, check every database result, and do not trigger notifications or remote updates until local persistence succeeds. Guard audio, D-Bus, process, and desktop integrations in test mode.

## Commit & Pull Request Guidelines

Follow the history's Conventional Commit pattern, such as `feat(tui): add shortcut` or `fix(qml): handle missing property`. Keep each commit scoped to one logical change. Pull requests should explain the user-visible effect, list verification commands, link relevant issues, and include screenshots or recordings for GUI/QML changes. Call out schema, packaging, or dependency changes explicitly.
