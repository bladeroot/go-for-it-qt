#pragma once

#include <QByteArray>
#include <QString>

namespace gofi {

class AppController;

enum class SettingsKeyResult {
    Handled,
    Close,
    EditValue
};

class TuiSettingsView {
public:
    explicit TuiSettingsView(AppController* controller = nullptr);

    void setController(AppController* controller);
    void reset();

    int selectedRow() const { return m_selectedRow; }
    void setSelectedRow(int row) { m_selectedRow = row; }

    SettingsKeyResult handleKey(const QByteArray& seq, QString& editInitialValue);
    QString applyEditValue(const QString& text);

    QString render(int bodyRows, int cols) const;

private:
    AppController* m_app = nullptr;
    int m_selectedRow = 0;
};

} // namespace gofi
