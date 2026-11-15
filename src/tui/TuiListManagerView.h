#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QVariantMap>

namespace gofi {

class AppController;

enum class ListPickerKeyResult {
    Handled,
    Close,
    EditList,
    Applied
};

enum class ListEditKeyResult {
    Handled,
    BackToPicker,
    EditField,
    SavedAndActivated
};

class TuiListManagerView {
public:
    explicit TuiListManagerView(AppController* controller = nullptr);

    void setController(AppController* controller);
    void openPicker();

    int pickerRow() const { return m_listPickerRow; }
    void setPickerRow(int row) { m_listPickerRow = row; }

    int editRow() const { return m_listEditRow; }
    void setEditRow(int row) { m_listEditRow = row; }

    bool isEditingPasswordField() const { return m_listEditRow == 4; }

    ListPickerKeyResult handlePickerKey(const QByteArray& seq, QString& statusMessage);
    ListEditKeyResult handleEditKey(const QByteArray& seq, QString& editInitialValue, QString& statusMessage);
    void applyFieldEdit(const QString& text);

    QString renderPicker(int bodyRows, int cols) const;
    QString renderEdit(int bodyRows, int cols) const;

private:
    AppController* m_app = nullptr;
    int m_listPickerRow = 0;
    QSet<QString> m_selectedListIds;
    int m_listEditRow = 0;
    QVariantMap m_editingListCfg;
};

} // namespace gofi
