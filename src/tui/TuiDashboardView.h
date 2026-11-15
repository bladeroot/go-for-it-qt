#pragma once

#include "core/AppController.h"
#include <QString>
#include <QByteArray>

namespace gofi {

class TuiDashboardView {
public:
    enum class Period {
        Day = 0,
        Week = 1,
        Month = 2,
        All = 3
    };

    enum class KeyResult {
        Ignored,
        Consumed,
        Close,
        ExportCsv,
        ExportPdf
    };

    explicit TuiDashboardView(AppController* controller);

    Period period() const { return m_period; }
    void setPeriod(Period period) { m_period = period; m_scroll = 0; }

    int scroll() const { return m_scroll; }
    void setScroll(int scroll) { m_scroll = scroll; }
    void resetScroll() { m_scroll = 0; }

    QString periodName() const;
    QString periodTitle() const;

    KeyResult handleKey(const QByteArray& seq);
    QString render(int bodyRows, int cols);

    static QString defaultBillingExportPath(const QString& filename);

private:
    AppController* m_app = nullptr;
    Period m_period = Period::Day;
    int m_scroll = 0;
};

} // namespace gofi
