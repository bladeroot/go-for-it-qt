#pragma once

#include <QSqlDatabase>

namespace gofi {

class DatabaseSchema {
public:
    static bool createSchema(QSqlDatabase database);
};

} // namespace gofi
