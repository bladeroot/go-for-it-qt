#pragma once

#include <QString>

namespace gofi {

class TuiApp;

class TuiCommandHandler {
public:
    explicit TuiCommandHandler(TuiApp* app);

    void execute(const QString& cmdLine);

private:
    TuiApp* m_tui = nullptr;
};

} // namespace gofi
