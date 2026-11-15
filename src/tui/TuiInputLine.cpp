#include "tui/TuiInputLine.h"
#include "tui/TuiUtils.h"
#include <QChar>

namespace gofi {

using namespace tui;

void TuiInputLine::setText(const QString& text) {
    m_text = text;
    m_cursor = static_cast<int>(m_text.size());
}

void TuiInputLine::setCursor(int pos) {
    m_cursor = qBound(0, pos, static_cast<int>(m_text.size()));
}

void TuiInputLine::clear() {
    m_text.clear();
    m_cursor = 0;
}

InputKeyResult TuiInputLine::handleKey(const QByteArray& seq) {
    // Esc cancels
    if (seq == "\033") {
        return InputKeyResult::Cancel;
    }
    // Enter commits
    if (seq == "\r" || seq == "\n") {
        return InputKeyResult::Commit;
    }
    // Backspace
    if (seq == "\x7f" || seq == "\x08") {
        if (m_cursor > 0) {
            int removeLen = 1;
            if (m_cursor >= 2 && m_text.at(m_cursor - 1).isLowSurrogate() && m_text.at(m_cursor - 2).isHighSurrogate()) {
                removeLen = 2;
            }
            m_text.remove(m_cursor - removeLen, removeLen);
            m_cursor -= removeLen;
            return InputKeyResult::TextChanged;
        }
        return InputKeyResult::Handled;
    }
    // Left / Right arrows
    if (seq == "\033[D") {
        if (m_cursor >= 2 && m_text.at(m_cursor - 1).isLowSurrogate() && m_text.at(m_cursor - 2).isHighSurrogate()) {
            m_cursor -= 2;
        } else {
            m_cursor = qMax(0, m_cursor - 1);
        }
        return InputKeyResult::Handled;
    }
    if (seq == "\033[C") {
        if (m_cursor + 1 < m_text.size() && m_text.at(m_cursor).isHighSurrogate() && m_text.at(m_cursor + 1).isLowSurrogate()) {
            m_cursor += 2;
        } else {
            m_cursor = qMin(static_cast<int>(m_text.size()), m_cursor + 1);
        }
        return InputKeyResult::Handled;
    }
    // Ctrl+U clears input line
    if (seq == "\x15") {
        m_text.clear();
        m_cursor = 0;
        return InputKeyResult::TextChanged;
    }

    // Printable UTF-8 character
    if (!seq.startsWith('\033')) {
        const QString ch = QString::fromUtf8(seq);
        if (!ch.isEmpty()) {
            const char32_t ucs4 = (ch.size() >= 2 && ch.at(0).isHighSurrogate() && ch.at(1).isLowSurrogate())
                                      ? QChar::surrogateToUcs4(ch.at(0), ch.at(1))
                                      : ch.at(0).unicode();
            if (QChar::isPrint(ucs4)) {
                m_text.insert(m_cursor, ch);
                m_cursor += static_cast<int>(ch.size());
                return InputKeyResult::TextChanged;
            }
        }
    }

    return InputKeyResult::Handled;
}

QString TuiInputLine::render(const QString& prompt, const QString& prefixColor, bool masked) const {
    QString displayText = m_text;
    if (masked) {
        displayText = QString(m_text.size(), QChar(0x2022));
    }
    return prefixColor + ESC_BOLD + prompt + ESC_RESET + displayText + QStringLiteral("█\033[K");
}

} // namespace gofi
