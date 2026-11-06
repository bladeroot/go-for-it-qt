#pragma once

#include <QHash>
#include <QString>
#include <QTranslator>

namespace gofi {

class UkTranslator : public QTranslator {
    Q_OBJECT
public:
    explicit UkTranslator(QObject* parent = nullptr);

    [[nodiscard]] bool isEmpty() const override { return false; }
    [[nodiscard]] QString translate(const char* context,
                                    const char* sourceText,
                                    const char* disambiguation = nullptr,
                                    int n = -1) const override;

private:
    QHash<QString, QString> m_dict;
};

} // namespace gofi
