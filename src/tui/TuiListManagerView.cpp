#include "tui/TuiListManagerView.h"
#include "core/AppController.h"
#include "core/ListManagerModel.h"
#include "tui/TuiUtils.h"
#include <QCoreApplication>

namespace gofi {

using namespace tui;

TuiListManagerView::TuiListManagerView(AppController* controller)
    : m_app(controller) {
}

void TuiListManagerView::setController(AppController* controller) {
    m_app = controller;
}

void TuiListManagerView::openPicker() {
    if (!m_app) return;
    m_listPickerRow = qMax(0, m_app->listManager()->indexOfId(m_app->currentListId()));
    m_selectedListIds.clear();
    if (m_app->isMultiListView()) {
        const QStringList active = m_app->activeListIds();
        for (const QString& id : active) {
            m_selectedListIds.insert(id);
        }
    }
}

ListPickerKeyResult TuiListManagerView::handlePickerKey(const QByteArray& seq, QString& statusMessage) {
    if (!m_app) return ListPickerKeyResult::Handled;
    const int nLists = m_app->listManager()->rowCount();

    if (seq == "\033" || seq == "q" || seq == "L") {
        return ListPickerKeyResult::Close;
    }
    if (seq == "j" || seq == "\033[B") {
        if (nLists > 0) m_listPickerRow = qMin(nLists - 1, m_listPickerRow + 1);
        return ListPickerKeyResult::Handled;
    }
    if (seq == "k" || seq == "\033[A") {
        if (nLists > 0) m_listPickerRow = qMax(0, m_listPickerRow - 1);
        return ListPickerKeyResult::Handled;
    }
    // Toggle multi-selection checkbox with Space
    if (seq == " ") {
        if (m_listPickerRow >= 0 && m_listPickerRow < nLists) {
            const QString id = m_app->listManager()->entry(m_listPickerRow).id;
            if (m_selectedListIds.contains(id)) {
                m_selectedListIds.remove(id);
            } else {
                m_selectedListIds.insert(id);
            }
        }
        return ListPickerKeyResult::Handled;
    }
    // Quick filter: All Lists
    if (seq == "*" || seq == "A") {
        m_app->showAllLists();
        statusMessage = QCoreApplication::translate("TuiApp", "Switched to All Lists (%1 lists).").arg(nLists);
        return ListPickerKeyResult::Applied;
    }
    // Quick filter: All Local SQLite lists
    if (seq == "l") {
        m_app->showProviderLists(QStringLiteral("SQLite"));
        statusMessage = QCoreApplication::translate("TuiApp", "Switched to Local lists view.");
        return ListPickerKeyResult::Applied;
    }
    // Quick filter: All Remote Trackers
    if (seq == "t" || seq == "T") {
        QStringList trackerIds;
        for (int i = 0; i < nLists; ++i) {
            const auto entry = m_app->listManager()->entry(i);
            if (entry.provider != QStringLiteral("SQLite")) {
                trackerIds.append(entry.id);
            }
        }
        if (!trackerIds.isEmpty()) {
            m_app->setMultiListFilter(trackerIds, QCoreApplication::translate("TuiApp", "All Trackers (%1)").arg(trackerIds.size()));
            statusMessage = QCoreApplication::translate("TuiApp", "Switched to All Trackers view (%1 lists).").arg(trackerIds.size());
            return ListPickerKeyResult::Applied;
        }
    }
    // Add list: a, o, n, +
    if (seq == "a" || seq == "o" || seq == "n" || seq == "+") {
        m_editingListCfg = m_app->listManager()->getListConfig(QString());
        m_editingListCfg[QStringLiteral("name")] = QStringLiteral("New List");
        m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Jira");
        m_listEditRow = 0;
        return ListPickerKeyResult::EditList;
    }
    // Edit list: e, i
    if ((seq == "e" || seq == "i") && m_listPickerRow >= 0 && m_listPickerRow < nLists) {
        const TodoListEntry entry = m_app->listManager()->entry(m_listPickerRow);
        m_editingListCfg = m_app->listManager()->getListConfig(entry.id);
        m_listEditRow = 0;
        return ListPickerKeyResult::EditList;
    }
    // Delete list: d
    if (seq == "d" && nLists > 1 && m_listPickerRow >= 0 && m_listPickerRow < nLists) {
        const TodoListEntry entry = m_app->listManager()->entry(m_listPickerRow);
        m_selectedListIds.remove(entry.id);
        m_app->listManager()->removeListById(entry.id);
        m_listPickerRow = qBound(0, m_listPickerRow, m_app->listManager()->rowCount() - 1);
        statusMessage = QCoreApplication::translate("TuiApp", "Removed list '%1'.").arg(entry.name);
        return ListPickerKeyResult::Handled;
    }
    // Apply selection: Enter
    if (seq == "\r" || seq == "\n") {
        if (m_selectedListIds.size() > 1) {
            QStringList ids = m_selectedListIds.values();
            m_app->setMultiListFilter(ids, QCoreApplication::translate("TuiApp", "Selected Lists (%1)").arg(ids.size()));
            statusMessage = QCoreApplication::translate("TuiApp", "Showing %1 selected lists.").arg(ids.size());
        } else if (m_selectedListIds.size() == 1) {
            const QString selId = *m_selectedListIds.begin();
            m_app->selectListById(selId);
            statusMessage = QCoreApplication::translate("TuiApp", "Switched to list '%1'.").arg(m_app->currentListName());
        } else if (m_listPickerRow >= 0 && m_listPickerRow < nLists) {
            const TodoListEntry entry = m_app->listManager()->entry(m_listPickerRow);
            m_app->selectListById(entry.id);
            statusMessage = QCoreApplication::translate("TuiApp", "Switched to list '%1' (%2).").arg(entry.name, entry.provider);
        }
        return ListPickerKeyResult::Applied;
    }
    return ListPickerKeyResult::Handled;
}

ListEditKeyResult TuiListManagerView::handleEditKey(const QByteArray& seq, QString& editInitialValue, QString& statusMessage) {
    if (seq == "\033" || seq == "q") {
        return ListEditKeyResult::BackToPicker;
    }
    if (seq == "j" || seq == "\033[B") {
        m_listEditRow = qMin(8, m_listEditRow + 1);
        return ListEditKeyResult::Handled;
    }
    if (seq == "k" || seq == "\033[A") {
        m_listEditRow = qMax(0, m_listEditRow - 1);
        return ListEditKeyResult::Handled;
    }
    if (seq == " " || seq == "\r" || seq == "\n" || seq == "e" || seq == "i" || seq == "h" || seq == "l") {
        if (m_listEditRow == 1) {
            // Cycle provider: SQLite -> Jira -> YouTrack -> GitHub -> GitLab -> Redmine -> Todoist -> Nextcloud
            const QString cur = m_editingListCfg.value(QStringLiteral("provider")).toString();
            if (cur == QStringLiteral("SQLite")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Jira");
            else if (cur == QStringLiteral("Jira")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("YouTrack");
            else if (cur == QStringLiteral("YouTrack")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("GitHub");
            else if (cur == QStringLiteral("GitHub")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("GitLab");
            else if (cur == QStringLiteral("GitLab")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Redmine");
            else if (cur == QStringLiteral("Redmine")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Todoist");
            else if (cur == QStringLiteral("Todoist")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Nextcloud");
            else m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("SQLite");
            return ListEditKeyResult::Handled;
        }
        if (m_listEditRow == 6) {
            m_editingListCfg[QStringLiteral("autoWorklog")] = !m_editingListCfg.value(QStringLiteral("autoWorklog"), true).toBool();
            return ListEditKeyResult::Handled;
        }
        if (m_listEditRow == 7) {
            m_editingListCfg[QStringLiteral("syncStatusToRemote")] = !m_editingListCfg.value(QStringLiteral("syncStatusToRemote"), false).toBool();
            return ListEditKeyResult::Handled;
        }
        if (m_listEditRow == 8) {
            if (!m_app) return ListEditKeyResult::Handled;
            const QVariantMap check = m_app->listManager()->validateAndCheckConflicts(m_editingListCfg);
            if (!check.value(QStringLiteral("valid")).toBool()) {
                statusMessage = check.value(QStringLiteral("error")).toString();
                return ListEditKeyResult::Handled;
            }
            const QString savedId = m_app->listManager()->saveListConfig(m_editingListCfg, QStringLiteral("keep"));
            m_app->selectListById(savedId);
            statusMessage = QCoreApplication::translate("TuiApp", "Saved and activated list '%1'.").arg(m_editingListCfg.value(QStringLiteral("name")).toString());
            return ListEditKeyResult::SavedAndActivated;
        }
        // Text fields (0, 2, 3, 4, 5)
        if (m_listEditRow == 0) editInitialValue = m_editingListCfg.value(QStringLiteral("name")).toString();
        else if (m_listEditRow == 2) editInitialValue = m_editingListCfg.value(QStringLiteral("jiraUrl")).toString();
        else if (m_listEditRow == 3) editInitialValue = m_editingListCfg.value(QStringLiteral("jiraUser")).toString();
        else if (m_listEditRow == 4) editInitialValue.clear();
        else if (m_listEditRow == 5) editInitialValue = m_editingListCfg.value(QStringLiteral("jiraJql")).toString();
        return ListEditKeyResult::EditField;
    }
    return ListEditKeyResult::Handled;
}

void TuiListManagerView::applyFieldEdit(const QString& text) {
    if (m_listEditRow == 0) m_editingListCfg[QStringLiteral("name")] = text;
    else if (m_listEditRow == 2) m_editingListCfg[QStringLiteral("jiraUrl")] = text;
    else if (m_listEditRow == 3) m_editingListCfg[QStringLiteral("jiraUser")] = text;
    else if (m_listEditRow == 4) {
        if (!text.isEmpty()) m_editingListCfg[QStringLiteral("apiToken")] = text;
    }
    else if (m_listEditRow == 5) m_editingListCfg[QStringLiteral("jiraJql")] = text;
}

QString TuiListManagerView::renderPicker(int bodyRows, int cols) const {
    if (!m_app) return QString();

    QString out;
    out += FG_CYAN + ESC_BOLD + QCoreApplication::translate("TuiApp", "  📋 LISTS (Space:toggle [x], Enter:apply, *:All, l:Local, t:Trackers, a/o:Add, e:Edit, d:Del, Esc:back):") + ESC_RESET + QStringLiteral("\033[K\r\n");

    const int nLists = m_app->listManager()->rowCount();
    for (int r = 1; r < bodyRows; ++r) {
        const int idx = r - 1;
        if (idx < nLists) {
            const TodoListEntry e = m_app->listManager()->entry(idx);
            const bool isSel = (idx == m_listPickerRow);
            const bool isChecked = m_selectedListIds.contains(e.id);
            const QString cursor = isSel ? QStringLiteral(" > ") : QStringLiteral("   ");
            const QString check = isChecked ? (FG_GREEN + QStringLiteral("[x]") + ESC_RESET) : (FG_GRAY + QStringLiteral("[ ]") + ESC_RESET);

            QString provColor = FG_WHITE;
            if (e.provider == QStringLiteral("GitHub")) provColor = FG_PURPLE;
            else if (e.provider == QStringLiteral("GitLab")) provColor = FG_ORANGE;
            else if (e.provider == QStringLiteral("Jira")) provColor = FG_CYAN;
            else if (e.provider == QStringLiteral("YouTrack")) provColor = FG_YELLOW;
            else if (e.provider == QStringLiteral("Redmine")) provColor = FG_RED;
            else if (e.provider == QStringLiteral("Todoist")) provColor = FG_ORANGE;
            else if (e.provider == QStringLiteral("Nextcloud")) provColor = FG_CYAN;
            else if (e.provider == QStringLiteral("SQLite")) provColor = FG_GREEN;

            const QString provBadge = provColor + QStringLiteral("[%1]").arg(padRight(e.provider, 8)) + ESC_RESET;
            const bool isCur = (e.id == m_app->currentListId() && !m_app->isMultiListView());
            const QString activeTag = isCur ? (FG_CYAN + QCoreApplication::translate("TuiApp", " (active)") + ESC_RESET) : QString();

            const QString line = QStringLiteral("%1 %2 %3 %4 %5")
                .arg(cursor, check, provBadge, sanitizeTerminalText(e.name), activeTag);
            out += (isSel ? (BG_SELECTED + FG_WHITE + ESC_BOLD) : FG_WHITE)
                + padRight(line, cols) + ESC_RESET + QStringLiteral("\033[K\r\n");
        } else {
            out += QStringLiteral("\033[K\r\n");
        }
    }
    return out;
}

QString TuiListManagerView::renderEdit(int bodyRows, int cols) const {
    QString out;
    out += FG_CYAN + ESC_BOLD + QCoreApplication::translate("TuiApp", "  ✎ EDIT LIST CONFIGURATION (j/k move, Enter/Space edit, q/Esc cancel):") + ESC_RESET + QStringLiteral("\033[K\r\n");

    const QString prov = m_editingListCfg.value(QStringLiteral("provider")).toString();
    const QString tokenRaw = m_editingListCfg.value(QStringLiteral("apiToken")).toString();
    const QString tokenMasked = tokenRaw.isEmpty()
        ? QCoreApplication::translate("TuiApp", "(empty)")
        : (tokenRaw.left(4) + QStringLiteral("••••••••"));

    const QString onStr = QCoreApplication::translate("TuiApp", "[ON]");
    const QString offStr = QCoreApplication::translate("TuiApp", "[OFF]");

    const QStringList editItems = {
        QCoreApplication::translate("TuiApp", "1. List Name                    : %1").arg(m_editingListCfg.value(QStringLiteral("name")).toString()),
        QCoreApplication::translate("TuiApp", "2. Provider (Enter to cycle)    : [%1] (SQLite / Jira / YouTrack / GitHub / GitLab)").arg(prov),
        QCoreApplication::translate("TuiApp", "3. Server URL                   : %1").arg(m_editingListCfg.value(QStringLiteral("jiraUrl")).toString()),
        QCoreApplication::translate("TuiApp", "4. Jira Email / Username        : %1").arg(m_editingListCfg.value(QStringLiteral("jiraUser")).toString()),
        QCoreApplication::translate("TuiApp", "5. API Token / Permanent Token  : %1").arg(tokenMasked),
        QCoreApplication::translate("TuiApp", "6. JQL / Search Query           : %1").arg(m_editingListCfg.value(QStringLiteral("jiraJql")).toString()),
        QCoreApplication::translate("TuiApp", "7. Auto-log Pomodoro Worklog    : %1").arg(m_editingListCfg.value(QStringLiteral("autoWorklog"), true).toBool() ? onStr : offStr),
        QCoreApplication::translate("TuiApp", "8. Sync Status Back to Tracker  : %1").arg(m_editingListCfg.value(QStringLiteral("syncStatusToRemote"), false).toBool() ? onStr : offStr),
        QCoreApplication::translate("TuiApp", "9. >>> [ SAVE & ACTIVATE LIST ] <<<")
    };

    for (int r = 1; r < bodyRows; ++r) {
        const int idx = r - 1;
        if (idx < editItems.size()) {
            const bool isSel = (idx == m_listEditRow);
            const QString prefix = isSel ? QStringLiteral("  > ") : QStringLiteral("    ");
            out += (isSel ? (BG_SELECTED + (idx == 8 ? FG_GREEN : FG_WHITE) + ESC_BOLD) : (idx == 8 ? FG_GREEN : FG_WHITE))
                + padRight(prefix + editItems.at(idx), cols) + ESC_RESET + QStringLiteral("\033[K\r\n");
        } else {
            out += QStringLiteral("\033[K\r\n");
        }
    }
    return out;
}

} // namespace gofi
