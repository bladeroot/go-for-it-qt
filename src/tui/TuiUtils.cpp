#include "tui/TuiUtils.h"
#include <QChar>
#include <QRegularExpression>
#include <algorithm>

namespace gofi {
namespace tui {

int codepointDisplayWidth(uint32_t cp) {
    if (cp == 0 || cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    // Combining marks and variation selectors
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0xFE00 && cp <= 0xFE0F)) return 0;
    // Wide emoji / symbols / CJK blocks
    if (cp >= 0x1F000 ||
        (cp >= 0x231A && cp <= 0x23FA) ||
        (cp >= 0x2614 && cp <= 0x2615) ||
        (cp >= 0x2648 && cp <= 0x2653) ||
        cp == 0x267F || cp == 0x2693 || cp == 0x26A1 ||
        (cp >= 0x26AA && cp <= 0x26AB) ||
        (cp >= 0x26BD && cp <= 0x26BE) ||
        (cp >= 0x26C4 && cp <= 0x26C5) ||
        cp == 0x26CE || cp == 0x26D4 || cp == 0x26EA ||
        (cp >= 0x26F2 && cp <= 0x26F5) ||
        cp == 0x26FA || cp == 0x26FD || cp == 0x2705 ||
        (cp >= 0x270A && cp <= 0x270B) ||
        cp == 0x2728 || cp == 0x274C || cp == 0x274E ||
        (cp >= 0x2753 && cp <= 0x2755) || cp == 0x2757 ||
        (cp >= 0x2795 && cp <= 0x2797) || cp == 0x27B0 || cp == 0x27BF ||
        (cp >= 0x2B1B && cp <= 0x2B1C) || cp == 0x2B50 || cp == 0x2B55 ||
        (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6)) {
        return 2;
    }
    return 1;
}

bool isAnsiEscapeAt(const QString& str, int index, int& escLen) {
    const int len = str.size();
    if (index >= len || str.at(index) != QLatin1Char('\033')) return false;
    if (index + 1 < len && str.at(index + 1) == QLatin1Char('[')) {
        int j = index + 2;
        while (j < len && str.at(j) >= QLatin1Char(' ') && str.at(j) <= QLatin1Char('?')) {
            ++j;
        }
        if (j < len && str.at(j) >= QLatin1Char('@') && str.at(j) <= QLatin1Char('~')) {
            escLen = j - index + 1;
            return true;
        }
    } else if (index + 1 < len && str.at(index + 1) == QLatin1Char(']')) {
        int j = index + 2;
        while (j < len) {
            if (str.at(j) == QLatin1Char('\a')) {
                escLen = j - index + 1;
                return true;
            }
            if (str.at(j) == QLatin1Char('\033') && j + 1 < len && str.at(j + 1) == QLatin1Char('\\')) {
                escLen = j - index + 2;
                return true;
            }
            ++j;
        }
    } else if (index + 1 < len && str.at(index + 1) >= QLatin1Char('@') && str.at(index + 1) <= QLatin1Char('_')) {
        escLen = 2;
        return true;
    }
    return false;
}

int displayWidth(const QString& str) {
    int w = 0;
    const int len = str.size();
    for (int i = 0; i < len; ++i) {
        int escLen = 0;
        if (isAnsiEscapeAt(str, i, escLen)) {
            i += escLen - 1;
            continue;
        }
        uint cp = str.at(i).unicode();
        if (str.at(i).isHighSurrogate() && i + 1 < len && str.at(i + 1).isLowSurrogate()) {
            cp = QChar::surrogateToUcs4(str.at(i), str.at(i + 1));
            ++i;
        }
        w += codepointDisplayWidth(cp);
    }
    return w;
}

QString truncateVisible(const QString& str, int maxCols) {
    if (maxCols <= 0) return {};
    if (displayWidth(str) <= maxCols) return str;
    if (maxCols <= 1) return QStringLiteral("…\033[0m");
    QString res;
    int curCols = 0;
    bool hasEscape = false;
    const int len = str.size();
    for (int i = 0; i < len; ++i) {
        int escLen = 0;
        if (isAnsiEscapeAt(str, i, escLen)) {
            res += str.mid(i, escLen);
            hasEscape = true;
            i += escLen - 1;
            continue;
        }
        uint cp = str.at(i).unicode();
        int step = 1;
        if (str.at(i).isHighSurrogate() && i + 1 < len && str.at(i + 1).isLowSurrogate()) {
            cp = QChar::surrogateToUcs4(str.at(i), str.at(i + 1));
            step = 2;
        }
        const int cw = codepointDisplayWidth(cp);
        if (curCols + cw > maxCols - 1) {
            break;
        }
        res += str.mid(i, step);
        curCols += cw;
        if (step == 2) ++i;
    }
    return res + (hasEscape ? QStringLiteral("…\033[0m") : QStringLiteral("…"));
}

QString padRight(const QString& str, int width) {
    const int dw = displayWidth(str);
    if (dw >= width) return truncateVisible(str, width);
    return str + QString(width - dw, QLatin1Char(' '));
}

QString formatSeconds(uint secs) {
    const uint h = secs / 3600U;
    const uint m = (secs / 60U) % 60U;
    const uint s = secs % 60U;
    if (h > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

QString sanitizeTerminalText(const QString& input, bool allowNewlines) {
    if (input.isEmpty()) return {};

    // Strip ANSI escape sequences:
    // - OSC sequences: ESC ] ... (BEL | ESC \)
    // - CSI sequences: ESC [ ... [@-~]
    // - DCS/APC/PM: ESC [P^_X] ... ESC \ or two-byte ESC .
    static const QRegularExpression oscRe(QStringLiteral("\x1b\\][^\x07\x1b]*(\x07|\x1b\\\\)"));
    static const QRegularExpression csiRe(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
    static const QRegularExpression escGeneralRe(QStringLiteral("\x1b[P^_X][^\x1b]*\x1b\\\\|\x1b."));

    QString clean = input;
    clean.remove(oscRe);
    clean.remove(csiRe);
    clean.remove(escGeneralRe);
    clean.remove(QLatin1Char('\x1b'));

    QString result;
    result.reserve(clean.size());
    for (int i = 0; i < clean.size(); ++i) {
        const QChar ch = clean.at(i);
        const ushort u = ch.unicode();

        if (allowNewlines && ch == QLatin1Char('\n')) {
            result.append(QLatin1Char('\n'));
        } else if (ch == QLatin1Char('\t')) {
            result.append(QStringLiteral("    "));
        } else if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) {
            if (ch == QLatin1Char('\r')) {
                continue;
            }
            result.append(QLatin1Char(' '));
        } else {
            result.append(ch);
        }
    }
    return result;
}

} // namespace tui
} // namespace gofi
