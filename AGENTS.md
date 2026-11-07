# Repository Guidelines

## Project Structure & Module Organization

The project is a C++20/Qt 6 productivity application with two front ends. Core services and models live in `src/core/`; Todo.txt parsing and storage are in `src/todotxt/`; the terminal UI is in `src/tui/`. Entry points are `src/main.cpp` for the Qt Quick GUI and `src/main_tui.cpp` for the TUI. QML views are grouped under `qml/components/`, `qml/dialogs/`, and `qml/pages/`. Runtime icons, sounds, desktop metadata, and AppStream metadata belong in `data/`. Tests live in `tests/`, while distribution files are under `packaging/`, `debian/`, and `PKGBUILD`.

## Build, Test, and Development Commands

Use an out-of-tree build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

Run `build/go-for-it-qt` for the GUI or `build/go-for-it-tui` for the terminal interface. Configure with `-DCMAKE_BUILD_TYPE=Release` for production builds. Install locally with `cmake --install build --prefix /usr/local`; avoid committing generated `build/` contents.

## Coding Style & Naming Conventions

Match existing Qt/C++ style: four-space indentation, braces on the same line, `PascalCase` for classes, and `camelCase` for methods and variables. Keep headers and implementations paired as `ClassName.h` and `ClassName.cpp`, use `QStringLiteral` for fixed Qt strings, and place application types in the `gofi` namespace. Name QML components in `PascalCase.qml`; use lower-camel-case properties and IDs. No repository-wide formatter is configured, so preserve nearby formatting and keep changes focused.

## Testing Guidelines

Tests use Qt Test and are registered through CTest. Add focused private slots to `tests/tst_todotxt.cpp`, naming them `testBehaviorOrFeature`. Exercise model state, persistence, and parsing with deterministic temporary data (`QTemporaryDir` where appropriate). Always run the offscreen CTest command above before submitting; add regression coverage for bug fixes.

## Commit & Pull Request Guidelines

Follow the history's Conventional Commit pattern, such as `feat(tui): add shortcut` or `fix(qml): handle missing property`. Keep each commit scoped to one logical change. Pull requests should explain the user-visible effect, list verification commands, link relevant issues, and include screenshots or recordings for GUI/QML changes. Call out schema, packaging, or dependency changes explicitly.
