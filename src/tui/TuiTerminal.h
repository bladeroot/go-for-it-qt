#pragma once

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>
#include <termios.h>

namespace gofi {

class TuiTerminal : public QObject {
    Q_OBJECT
public:
    explicit TuiTerminal(QObject* parent = nullptr);
    ~TuiTerminal() override;

    bool initialize();
    void enableRawMode();
    void disableRawMode();
    bool isRawMode() const { return m_rawModeActive; }

    void setSuspended(bool suspended);

    int rows() const { return m_rows; }
    int cols() const { return m_cols; }
    bool updateTerminalSize();

    void write(const QString& str);
    void write(const QByteArray& bytes);

public slots:
    void onStdinActivated();

signals:
    void keySequenceReceived(const QByteArray& seq);
    void terminalResized(int rows, int cols);
    void redrawRequested();

private slots:
    void onEscTimeout();
    void onResizeTimeout();

private:
    void processPendingStdin();

    struct termios m_origTermios {};
    bool m_rawModeActive = false;
    int m_rows = 24;
    int m_cols = 80;

    QSocketNotifier* m_stdinNotifier = nullptr;
    QTimer m_resizeTimer;
    QTimer m_escTimer;
    QByteArray m_stdinPending;
};

} // namespace gofi
