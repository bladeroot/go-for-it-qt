#pragma once

#include <QAbstractListModel>
#include <QSettings>
#include <QVector>

namespace gofi {

struct ShortcutEntry {
    QString id;
    QString description;
    QString defaultSequence;
    QString sequence;
};

class ShortcutManager : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        DescriptionRole,
        SequenceRole,
        ReadableSequenceRole
    };

    explicit ShortcutManager(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QString shortcutFor(const QString& shortcutId) const;
    Q_INVOKABLE void setShortcut(const QString& shortcutId, const QString& sequence);
    Q_INVOKABLE void resetDefaults();

signals:
    void shortcutsChanged();

private:
    void loadShortcuts();
    void saveShortcut(const QString& id, const QString& seq);

    QSettings m_settings;
    QVector<ShortcutEntry> m_shortcuts;
};

} // namespace gofi
