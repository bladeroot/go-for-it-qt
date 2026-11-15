#pragma once

#include <QString>
#include <functional>
#include <optional>

namespace gofi {

class TuiExternalEditor {
public:
    using TerminalPauseResume = std::function<void(bool paused)>;

    static QString resolveEditorBinary();
    static bool hasExternalEditor();

    static std::optional<QString> editContent(
        const QString& initialText,
        const QString& tempFileTemplate,
        const TerminalPauseResume& pauseResume = nullptr,
        bool* noEditorFound = nullptr
    );
};

} // namespace gofi
