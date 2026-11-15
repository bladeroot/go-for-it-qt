#pragma once

#include <QString>
#include <cstdint>

namespace gofi {
namespace tui {

// ANSI Escape Helpers
inline const QString ESC_CLEAR_HOME = QStringLiteral("\033[H");
inline const QString ESC_CLEAR_ALL  = QStringLiteral("\033[2J\033[H");
inline const QString ESC_HIDE_CUR   = QStringLiteral("\033[?25l");
inline const QString ESC_SHOW_CUR   = QStringLiteral("\033[?25h");
inline const QString ESC_ALT_ON     = QStringLiteral("\033[?1049h");
inline const QString ESC_ALT_OFF    = QStringLiteral("\033[?1049l");
inline const QString ESC_RESET      = QStringLiteral("\033[0m");
inline const QString ESC_BOLD       = QStringLiteral("\033[1m");
inline const QString ESC_DIM        = QStringLiteral("\033[2m");
inline const QString ESC_STRIKE     = QStringLiteral("\033[9m");

inline const QString FG_CYAN        = QStringLiteral("\033[38;5;81m");
inline const QString FG_GREEN       = QStringLiteral("\033[38;5;114m");
inline const QString FG_YELLOW      = QStringLiteral("\033[38;5;221m");
inline const QString FG_ORANGE      = QStringLiteral("\033[38;5;209m");
inline const QString FG_RED         = QStringLiteral("\033[38;5;203m");
inline const QString FG_PURPLE      = QStringLiteral("\033[38;5;177m");
inline const QString FG_GRAY        = QStringLiteral("\033[38;5;244m");
inline const QString FG_WHITE       = QStringLiteral("\033[38;5;255m");

inline const QString BG_HEADER      = QStringLiteral("\033[48;5;236m");
inline const QString BG_SELECTED    = QStringLiteral("\033[48;5;238m");
inline const QString BG_ACTIVE_TAB  = QStringLiteral("\033[48;5;31m\033[38;5;255m\033[1m");
inline const QString BG_MODE_NORM   = QStringLiteral("\033[48;5;31m\033[38;5;255m\033[1m");
inline const QString BG_MODE_INS    = QStringLiteral("\033[48;5;28m\033[38;5;255m\033[1m");
inline const QString BG_MODE_CMD    = QStringLiteral("\033[48;5;130m\033[38;5;255m\033[1m");

int codepointDisplayWidth(uint32_t cp);
bool isAnsiEscapeAt(const QString& str, int index, int& escLen);
int displayWidth(const QString& str);
QString truncateVisible(const QString& str, int maxCols);
QString padRight(const QString& str, int width);
QString formatSeconds(uint secs);
QString sanitizeTerminalText(const QString& input, bool allowNewlines = false);

} // namespace tui
} // namespace gofi
