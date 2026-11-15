#include "tui/TuiMarkdownView.h"
#include "tui/TuiUtils.h"
#include <QCoreApplication>
#include <QRegularExpression>
#include <algorithm>

namespace gofi {

using namespace tui;

QString TuiMarkdownView::formatInlineMarkdown(const QString& text) {
    QString s = text;
    // Links: [label](url)
    static const QRegularExpression linkRe(QStringLiteral("\\[([^\\]]+)\\]\\(([^\\)]+)\\)"));
    s.replace(linkRe, QStringLiteral("\033[4m\033[38;5;81m\\1\033[0m\033[38;5;244m (\\2)\033[38;5;255m"));
    // Inline code: `code`
    static const QRegularExpression codeRe(QStringLiteral("`([^`]+)`"));
    s.replace(codeRe, QStringLiteral("\033[48;5;236m\033[38;5;209m \\1 \033[0m\033[38;5;255m"));
    // Bold: **text**
    static const QRegularExpression boldRe(QStringLiteral("\\*\\*([^\\*]+)\\*\\*"));
    s.replace(boldRe, QStringLiteral("\033[1m\033[38;5;255m\\1\033[22m"));
    // Italic: *text*
    static const QRegularExpression italicRe(QStringLiteral("(?<!\\*)\\*([^\\*]+)\\*(?!\\*)"));
    s.replace(italicRe, QStringLiteral("\033[3m\033[38;5;114m\\1\033[23m\033[38;5;255m"));
    return s;
}

QStringList TuiMarkdownView::renderMarkdownToAnsi(const QString& markdown, int maxWidth) {
    QStringList out;
    const int wrapCols = std::max(20, maxWidth - 4);
    const QString safeMarkdown = sanitizeTerminalText(markdown, true);
    const QStringList rawLines = safeMarkdown.split(QLatin1Char('\n'));
    bool inCodeBlock = false;
    QString codeLang;

    for (const QString& rawLine : rawLines) {
        const QString trimmed = rawLine.trimmed();

        // Fenced code blocks ```
        if (trimmed.startsWith(QStringLiteral("```"))) {
            if (!inCodeBlock) {
                inCodeBlock = true;
                codeLang = trimmed.mid(3).trimmed();
                const QString label = codeLang.isEmpty() ? QStringLiteral(" code ") : QStringLiteral(" %1 ").arg(codeLang);
                const int dashLen = std::max(2, wrapCols - static_cast<int>(label.size()) - 4);
                out.append(FG_GRAY + QStringLiteral("  ╭─") + FG_YELLOW + label + FG_GRAY + QString(dashLen, QChar(0x2500)) + QStringLiteral("╮") + ESC_RESET);
            } else {
                inCodeBlock = false;
                out.append(FG_GRAY + QStringLiteral("  ╰") + QString(std::max(2, wrapCols - 2), QChar(0x2500)) + QStringLiteral("╯") + ESC_RESET);
            }
            continue;
        }

        if (inCodeBlock) {
            out.append(FG_GRAY + QStringLiteral("  │ ") + FG_ORANGE + truncateVisible(rawLine, wrapCols - 4) + ESC_RESET);
            continue;
        }

        if (trimmed.isEmpty()) {
            out.append(QString());
            continue;
        }

        // Horizontal rule
        if (trimmed == QStringLiteral("---") || trimmed == QStringLiteral("***") || trimmed == QStringLiteral("___")) {
            out.append(FG_GRAY + QStringLiteral("  ") + QString(wrapCols, QChar(0x2500)) + ESC_RESET);
            continue;
        }

        // Headings (#, ##, ###)
        if (trimmed.startsWith(QLatin1Char('#'))) {
            int level = 0;
            while (level < trimmed.size() && trimmed.at(level) == QLatin1Char('#')) {
                ++level;
            }
            const QString hText = truncateVisible(trimmed.mid(level).trimmed(), wrapCols - 4);
            if (level == 1) {
                out.append(ESC_BOLD + FG_CYAN + QStringLiteral("  █ ") + hText + ESC_RESET);
                out.append(FG_CYAN + QStringLiteral("  ") + QString(std::min(wrapCols, static_cast<int>(hText.size()) + 4), QChar(0x2550)) + ESC_RESET);
            } else if (level == 2) {
                out.append(ESC_BOLD + FG_YELLOW + QStringLiteral("  ◆ ") + hText + ESC_RESET);
            } else {
                out.append(ESC_BOLD + FG_PURPLE + QStringLiteral("  ▸ ") + hText + ESC_RESET);
            }
            continue;
        }

        // Blockquotes (> )
        if (trimmed.startsWith(QStringLiteral("> "))) {
            const QString qBody = trimmed.mid(2);
            out.append(FG_CYAN + QStringLiteral("  ┃ ") + FG_GRAY + formatInlineMarkdown(truncateVisible(qBody, wrapCols - 4)) + ESC_RESET);
            continue;
        }

        // Task list checkboxes (- [x] / - [ ])
        if (trimmed.startsWith(QStringLiteral("- [x] "), Qt::CaseInsensitive) || trimmed.startsWith(QStringLiteral("* [x] "), Qt::CaseInsensitive)) {
            out.append(FG_GREEN + QStringLiteral("  ☑ ") + FG_GRAY + ESC_STRIKE + truncateVisible(trimmed.mid(6), wrapCols - 4) + ESC_RESET);
            continue;
        }
        if (trimmed.startsWith(QStringLiteral("- [ ] ")) || trimmed.startsWith(QStringLiteral("* [ ] "))) {
            out.append(FG_YELLOW + QStringLiteral("  ☐ ") + FG_WHITE + formatInlineMarkdown(truncateVisible(trimmed.mid(6), wrapCols - 4)) + ESC_RESET);
            continue;
        }

        // Bullet lists (- / * / •)
        if (trimmed.startsWith(QStringLiteral("- ")) || trimmed.startsWith(QStringLiteral("* ")) || trimmed.startsWith(QStringLiteral("• "))) {
            const QString itemBody = trimmed.mid(2);
            out.append(FG_CYAN + QStringLiteral("  • ") + FG_WHITE + formatInlineMarkdown(truncateVisible(itemBody, wrapCols - 4)) + ESC_RESET);
            continue;
        }

        // Markdown tables (| ... |)
        if (trimmed.startsWith(QLatin1Char('|')) && trimmed.endsWith(QLatin1Char('|'))) {
            QString tableLine = trimmed;
            tableLine.replace(QLatin1Char('|'), FG_CYAN + QStringLiteral(" │ ") + FG_WHITE);
            out.append(truncateVisible(QStringLiteral("  ") + tableLine + ESC_RESET, wrapCols));
            continue;
        }

        // Word-wrap normal paragraph lines
        QString rem = rawLine;
        while (rem.size() > wrapCols) {
            int splitPos = rem.lastIndexOf(QLatin1Char(' '), wrapCols);
            if (splitPos <= 0) splitPos = wrapCols;
            out.append(QStringLiteral("  ") + FG_WHITE + formatInlineMarkdown(rem.left(splitPos).trimmed()) + ESC_RESET);
            rem = rem.mid(splitPos).trimmed();
        }
        if (!rem.isEmpty()) {
            out.append(QStringLiteral("  ") + FG_WHITE + formatInlineMarkdown(rem) + ESC_RESET);
        }
    }
    return out;
}

TuiMarkdownView::KeyResult TuiMarkdownView::handleKey(const QByteArray& seq) {
    if (seq == "\033" || seq == "q" || seq == "v" || seq == "M") {
        return KeyResult::Close;
    }
    if (seq == "j" || seq == "\033[B" || seq == "\r" || seq == "\n") {
        ++m_scroll;
        return KeyResult::Consumed;
    }
    if (seq == "k" || seq == "\033[A") {
        m_scroll = std::max(0, m_scroll - 1);
        return KeyResult::Consumed;
    }
    if (seq == "d" || seq == "\x04" || seq == " " || seq == "\033[6~") {
        m_scroll += 10;
        return KeyResult::Consumed;
    }
    if (seq == "u" || seq == "\x15" || seq == "\033[5~") {
        m_scroll = std::max(0, m_scroll - 10);
        return KeyResult::Consumed;
    }
    if (seq == "g") {
        m_scroll = 0;
        return KeyResult::Consumed;
    }
    if (seq == "G") {
        m_scroll = 100000;
        return KeyResult::Consumed;
    }
    if (seq == "E" || seq == "e") {
        return KeyResult::EditDescription;
    }
    if (seq == "C" || seq == "c") {
        return KeyResult::AddComment;
    }
    if (seq == "w" || seq == "gx") {
        return KeyResult::OpenBrowser;
    }
    return KeyResult::Ignored;
}

QString TuiMarkdownView::render(int bodyRows, int cols) {
    QString out;
    const QString headerText = QCoreApplication::translate(
        "TuiApp",
        " 📖 MARKDOWN VIEWER: %1  (j/k/d/u/g/G scroll, E edit desc, C add comment, q/Esc close)"
    ).arg(m_title);

    out += BG_HEADER + FG_CYAN + ESC_BOLD
        + padRight(headerText, cols)
        + ESC_RESET + QStringLiteral("\033[K\r\n");

    const QStringList mdLines = renderMarkdownToAnsi(m_content, cols);
    const int availRows = bodyRows - 1;
    const int maxScroll = std::max(0, static_cast<int>(mdLines.size()) - availRows);
    m_scroll = std::clamp(m_scroll, 0, maxScroll);

    for (int r = 0; r < availRows; ++r) {
        const int idx = m_scroll + r;
        if (idx < mdLines.size()) {
            out += truncateVisible(mdLines.at(idx), cols) + QStringLiteral("\033[K\r\n");
        } else {
            out += FG_GRAY + QStringLiteral(" ~") + ESC_RESET + QStringLiteral("\033[K\r\n");
        }
    }
    return out;
}

} // namespace gofi
