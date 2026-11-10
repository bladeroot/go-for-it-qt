#include "ShortcutManager.h"
#include <QKeySequence>

namespace gofi {

ShortcutManager::ShortcutManager(QObject* parent)
    : QAbstractListModel(parent)
    , m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("shortcuts"))
{
    loadShortcuts();
}

int ShortcutManager::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_shortcuts.size());
}

QVariant ShortcutManager::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_shortcuts.size()) {
        return {};
    }
    const ShortcutEntry& e = m_shortcuts.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case DescriptionRole:
        return e.description;
    case IdRole:
        return e.id;
    case SequenceRole:
        return e.sequence;
    case ReadableSequenceRole:
        return e.sequence.isEmpty()
            ? tr("Disabled")
            : QKeySequence(e.sequence).toString(QKeySequence::NativeText);
    default:
        return {};
    }
}

QHash<int, QByteArray> ShortcutManager::roleNames() const {
    return {
        {IdRole, "shortcutId"},
        {DescriptionRole, "description"},
        {SequenceRole, "sequence"},
        {ReadableSequenceRole, "readableSequence"}
    };
}

QVariantMap ShortcutManager::shortcutMap() const {
    QVariantMap map;
    for (const ShortcutEntry& e : m_shortcuts) {
        map.insert(e.id, e.sequence);
    }
    return map;
}

QString ShortcutManager::shortcutFor(const QString& shortcutId) const {
    for (const ShortcutEntry& e : m_shortcuts) {
        if (e.id == shortcutId) {
            return e.sequence;
        }
    }
    return {};
}

void ShortcutManager::setShortcut(const QString& shortcutId, const QString& sequence) {
    const QString normalized = sequence.trimmed().isEmpty()
        ? QString()
        : QKeySequence(sequence).toString(QKeySequence::PortableText);

    // Clear conflicts first (just like KeyBindingSettings::change_shortcut in Vala)
    if (!normalized.isEmpty()) {
        for (int i = 0; i < m_shortcuts.size(); ++i) {
            if (m_shortcuts[i].id != shortcutId && m_shortcuts[i].sequence.compare(normalized, Qt::CaseInsensitive) == 0) {
                m_shortcuts[i].sequence.clear();
                saveShortcut(m_shortcuts[i].id, QString());
                const QModelIndex idx = index(i, 0);
                emit dataChanged(idx, idx, {SequenceRole, ReadableSequenceRole});
            }
        }
    }

    for (int i = 0; i < m_shortcuts.size(); ++i) {
        if (m_shortcuts[i].id == shortcutId) {
            if (m_shortcuts[i].sequence == normalized) return;
            m_shortcuts[i].sequence = normalized;
            saveShortcut(shortcutId, normalized);
            const QModelIndex idx = index(i, 0);
            emit dataChanged(idx, idx, {SequenceRole, ReadableSequenceRole});
            emit shortcutsChanged();
            return;
        }
    }
}

void ShortcutManager::resetDefaults() {
    beginResetModel();
    for (ShortcutEntry& e : m_shortcuts) {
        e.sequence = e.defaultSequence;
        saveShortcut(e.id, e.sequence);
    }
    endResetModel();
    emit shortcutsChanged();
}

void ShortcutManager::loadShortcuts() {
    m_shortcuts = {
        {QStringLiteral("filter"),             tr("Filter tasks"),                           QStringLiteral("Ctrl+F"),         {}},
        {QStringLiteral("sort"),               tr("Sort tasks"),                             QStringLiteral("Ctrl+S"),         {}},
        {QStringLiteral("add-new"),            tr("Add new task / list"),                    QStringLiteral("Ctrl+N"),         {}},
        {QStringLiteral("toggle-timer"),       tr("Start / Stop the timer"),                 QStringLiteral("Ctrl+T"),         {}},
        {QStringLiteral("skip"),               tr("Skip the break or skip to the break"),    QStringLiteral("Ctrl+K"),         {}},
        {QStringLiteral("edit-properties"),    tr("Edit the properties of a list or task"),  QStringLiteral("F2"),             {}},
        {QStringLiteral("mark-task-done"),     tr("Mark the task as complete"),              QStringLiteral("Ctrl+D"),         {}},
        {QStringLiteral("move-row-up"),        tr("Move selected row up"),                   QStringLiteral("Ctrl+Up"),        {}},
        {QStringLiteral("move-row-down"),      tr("Move selected row down"),                 QStringLiteral("Ctrl+Down"),      {}},
        {QStringLiteral("next-task"),          tr("Move to next task / row"),                QStringLiteral("Alt+Down"),       {}},
        {QStringLiteral("prev-task"),          tr("Move to previous task / row"),            QStringLiteral("Alt+Up"),         {}},
        {QStringLiteral("cycle-page"),         tr("Move to right screen"),                   QStringLiteral("Ctrl+PageDown"),  {}},
        {QStringLiteral("cycle-page-reverse"), tr("Move to left screen"),                    QStringLiteral("Ctrl+PageUp"),    {}}
    };

    for (ShortcutEntry& e : m_shortcuts) {
        e.sequence = m_settings.value(QStringLiteral("Shortcuts/") + e.id, e.defaultSequence).toString();
    }
}

void ShortcutManager::saveShortcut(const QString& id, const QString& seq) {
    m_settings.setValue(QStringLiteral("Shortcuts/") + id, seq);
}

} // namespace gofi
