#pragma once

#include <QString>

namespace gofi {

class DatabaseManager;

class DatabaseBackup {
public:
    explicit DatabaseBackup(DatabaseManager* db = nullptr);

    bool relocateDatabase(const QString& newDbPath, bool copyExisting = true);
    bool exportDatabaseBackup(const QString& destinationFilePath) const;
    bool importDatabaseBackup(const QString& sourceFilePath);

    static bool isValidGofiDatabase(const QString& filePath);
    static bool areSameFiles(const QString& pathA, const QString& pathB);
    static QString expandUserPath(const QString& path);

private:
    DatabaseManager* m_dbManager = nullptr;
};

} // namespace gofi
