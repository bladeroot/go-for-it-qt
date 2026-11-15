#pragma once

#include <QByteArray>
#include <QString>

namespace gofi {

enum class InputKeyResult {
    Handled,
    Cancel,
    Commit,
    TextChanged
};

class TuiInputLine {
public:
    TuiInputLine() = default;

    void setText(const QString& text);
    const QString& text() const { return m_text; }
    QString trimmedText() const { return m_text.trimmed(); }

    int cursor() const { return m_cursor; }
    void setCursor(int pos);

    void clear();
    bool isEmpty() const { return m_text.isEmpty(); }

    InputKeyResult handleKey(const QByteArray& seq);

    QString render(const QString& prompt, const QString& prefixColor, bool masked = false) const;

private:
    QString m_text;
    int m_cursor = 0;
};

} // namespace gofi
