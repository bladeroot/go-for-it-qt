#pragma once

#include <QSqlDatabase>

namespace gofi {

class DatabaseSchema {
public:
    static constexpr int CurrentSchemaVersion = 5;

    static bool createSchema(QSqlDatabase database);
    [[nodiscard]] static int userVersion(QSqlDatabase database);
    static bool setUserVersion(QSqlDatabase database, int version);

private:
    static bool initializeFreshDatabase(QSqlDatabase database);
    static bool migrateLegacyDatabase(QSqlDatabase database);
    static bool ensureUniqueJiraIndex(QSqlDatabase database);
};

} // namespace gofi
