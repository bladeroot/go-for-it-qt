#include "tui/TuiExternalEditor.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace gofi {

QString TuiExternalEditor::resolveEditorBinary() {
    QString editor = qEnvironmentVariable("VISUAL").trimmed();
    if (editor.isEmpty()) {
        editor = qEnvironmentVariable("EDITOR").trimmed();
    }
    if (editor.isEmpty()) {
        for (const QString& cand : {QStringLiteral("nvim"), QStringLiteral("vim"), QStringLiteral("vi"), QStringLiteral("nano")}) {
            if (!QStandardPaths::findExecutable(cand).isEmpty()) {
                editor = cand;
                break;
            }
        }
    }
    return editor;
}

bool TuiExternalEditor::hasExternalEditor() {
    return !resolveEditorBinary().isEmpty();
}

std::optional<QString> TuiExternalEditor::editContent(
    const QString& initialText,
    const QString& tempFileTemplate,
    const TerminalPauseResume& pauseResume,
    bool* noEditorFound
) {
    if (noEditorFound) {
        *noEditorFound = false;
    }

    const QString editor = resolveEditorBinary();
    if (editor.isEmpty()) {
        if (noEditorFound) {
            *noEditorFound = true;
        }
        return std::nullopt;
    }

    const QString templatePattern = tempFileTemplate.isEmpty()
        ? QStringLiteral("gofi-edit-XXXXXX.md")
        : tempFileTemplate;

    QTemporaryFile tempFile(QDir::tempPath() + QLatin1Char('/') + templatePattern);
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        return std::nullopt;
    }
    tempFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!initialText.isEmpty()) {
        tempFile.write(initialText.toUtf8());
        tempFile.flush();
    }
    const QString tmpPath = tempFile.fileName();
    tempFile.close();

    if (pauseResume) {
        pauseResume(true);
    }

    const int rc = QProcess::execute(
        QStringLiteral("/bin/sh"),
        {QStringLiteral("-c"), editor + QStringLiteral(" \"") + tmpPath + QStringLiteral("\"")}
    );

    if (pauseResume) {
        pauseResume(false);
    }

    std::optional<QString> result = std::nullopt;
    QFile in(tmpPath);
    if (rc == 0 && in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result = QString::fromUtf8(in.readAll());
        in.close();
    }
    QFile::remove(tmpPath);
    return result;
}

} // namespace gofi
