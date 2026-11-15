#include "tui/TuiTerminal.h"
#include "tui/TuiUtils.h"
#include <sys/ioctl.h>
#include <unistd.h>
#include <algorithm>

namespace gofi {

TuiTerminal::TuiTerminal(QObject* parent)
    : QObject(parent) {
    updateTerminalSize();
    m_escTimer.setSingleShot(true);
    connect(&m_escTimer, &QTimer::timeout, this, &TuiTerminal::onEscTimeout);
    connect(&m_resizeTimer, &QTimer::timeout, this, &TuiTerminal::onResizeTimeout);
}

TuiTerminal::~TuiTerminal() {
    disableRawMode();
}

bool TuiTerminal::initialize() {
    enableRawMode();
    updateTerminalSize();
    if (!m_stdinNotifier) {
        m_stdinNotifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
        connect(m_stdinNotifier, &QSocketNotifier::activated, this, &TuiTerminal::onStdinActivated);
    }
    m_resizeTimer.start(250);
    return true;
}

void TuiTerminal::enableRawMode() {
    if (m_rawModeActive) return;
    if (::tcgetattr(STDIN_FILENO, &m_origTermios) == 0) {
        struct termios raw = m_origTermios;
        raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        m_rawModeActive = true;
    }
    const QByteArray initSeq = (tui::ESC_ALT_ON + tui::ESC_HIDE_CUR + tui::ESC_CLEAR_ALL).toUtf8();
    (void)::write(STDOUT_FILENO, initSeq.constData(), static_cast<size_t>(initSeq.size()));
}

void TuiTerminal::disableRawMode() {
    if (!m_rawModeActive) return;
    const QByteArray exitSeq = (tui::ESC_RESET + tui::ESC_SHOW_CUR + tui::ESC_ALT_OFF).toUtf8();
    (void)::write(STDOUT_FILENO, exitSeq.constData(), static_cast<size_t>(exitSeq.size()));
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &m_origTermios);
    m_rawModeActive = false;
}

void TuiTerminal::setSuspended(bool suspended) {
    if (suspended) {
        if (m_stdinNotifier) m_stdinNotifier->setEnabled(false);
        disableRawMode();
    } else {
        enableRawMode();
        updateTerminalSize();
        if (m_stdinNotifier) m_stdinNotifier->setEnabled(true);
    }
}

bool TuiTerminal::updateTerminalSize() {
    struct winsize ws {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        const int newRows = qMax(14, static_cast<int>(ws.ws_row));
        const int newCols = qMax(50, static_cast<int>(ws.ws_col));
        if (newRows != m_rows || newCols != m_cols) {
            m_rows = newRows;
            m_cols = newCols;
            return true;
        }
    }
    return false;
}

void TuiTerminal::write(const QString& str) {
    const QByteArray bytes = str.toUtf8();
    (void)::write(STDOUT_FILENO, bytes.constData(), static_cast<size_t>(bytes.size()));
}

void TuiTerminal::write(const QByteArray& bytes) {
    (void)::write(STDOUT_FILENO, bytes.constData(), static_cast<size_t>(bytes.size()));
}

void TuiTerminal::onStdinActivated() {
    char buf[128];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return;

    m_escTimer.stop();
    m_stdinPending.append(buf, static_cast<int>(n));
    processPendingStdin();
    emit redrawRequested();
}

void TuiTerminal::onEscTimeout() {
    if (!m_stdinPending.isEmpty() && m_stdinPending.at(0) == '\033') {
        emit keySequenceReceived(QByteArray("\033"));
        m_stdinPending.remove(0, 1);
        processPendingStdin();
        emit redrawRequested();
    }
}

void TuiTerminal::onResizeTimeout() {
    if (updateTerminalSize()) {
        emit terminalResized(m_rows, m_cols);
        emit redrawRequested();
    }
}

void TuiTerminal::processPendingStdin() {
    int pos = 0;
    while (pos < m_stdinPending.size()) {
        const unsigned char b = static_cast<unsigned char>(m_stdinPending.at(pos));

        if (b == '\033') {
            if (pos + 1 >= m_stdinPending.size()) {
                // Lone trailing '\033' at the end of buffer
                if (!m_escTimer.isActive()) {
                    m_escTimer.start(50);
                }
                break;
            }

            const char next = m_stdinPending.at(pos + 1);
            if (next == '[') {
                // CSI sequence: '\033[' [0x20-0x3F]* [0x40-0x7E]
                int idx = pos + 2;
                bool foundFinal = false;
                while (idx < m_stdinPending.size()) {
                    const unsigned char c = static_cast<unsigned char>(m_stdinPending.at(idx));
                    if (c >= 0x40 && c <= 0x7E) {
                        foundFinal = true;
                        ++idx;
                        break;
                    } else if (c >= 0x20 && c <= 0x3F) {
                        ++idx;
                        if (idx - pos > 32) {
                            foundFinal = true;
                            break;
                        }
                    } else {
                        foundFinal = true;
                        break;
                    }
                }

                if (!foundFinal) {
                    // Incomplete trailing CSI sequence
                    break;
                }

                const QByteArray seq = m_stdinPending.mid(pos, idx - pos);
                pos = idx;
                m_escTimer.stop();
                emit keySequenceReceived(seq);
                continue;
            } else if (next == 'O') {
                // SS3 sequence: '\033O' [0x20-0x7E]
                if (pos + 2 >= m_stdinPending.size()) {
                    break;
                }
                const QByteArray seq = m_stdinPending.mid(pos, 3);
                pos += 3;
                m_escTimer.stop();
                emit keySequenceReceived(seq);
                continue;
            } else {
                // Lone '\033' followed by non-sequence character
                m_escTimer.stop();
                emit keySequenceReceived(QByteArray("\033"));
                pos += 1;
                continue;
            }
        }

        // UTF-8 code point
        int expectedLen = 1;
        if ((b & 0x80) == 0) {
            expectedLen = 1;
        } else if ((b & 0xE0) == 0xC0) {
            expectedLen = 2;
        } else if ((b & 0xF0) == 0xE0) {
            expectedLen = 3;
        } else if ((b & 0xF8) == 0xF0) {
            expectedLen = 4;
        }

        bool validContinuation = true;
        for (int k = 1; k < expectedLen && (pos + k < m_stdinPending.size()); ++k) {
            const unsigned char cb = static_cast<unsigned char>(m_stdinPending.at(pos + k));
            if ((cb & 0xC0) != 0x80) {
                validContinuation = false;
                break;
            }
        }

        if (!validContinuation) {
            expectedLen = 1;
        } else if (pos + expectedLen > m_stdinPending.size()) {
            break;
        }

        const QByteArray token = m_stdinPending.mid(pos, expectedLen);
        pos += expectedLen;

        emit keySequenceReceived(token);
    }

    if (pos > 0) {
        m_stdinPending.remove(0, pos);
    }
}

} // namespace gofi
