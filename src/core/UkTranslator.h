#pragma once

#include <QHash>
#include <QString>
#include <QTranslator>

namespace gofi {

class UkTranslator : public QTranslator {
    Q_OBJECT
public:
    explicit UkTranslator(QObject* parent = nullptr);

    void setLanguageCode(const QString& code);
    [[nodiscard]] QString languageCode() const { return m_langCode; }

    bool loadTranslationsFromJson(const QString& filePath, const QString& langCode);
    void loadTranslationsFromDirectory(const QString& dirPath);

    [[nodiscard]] bool isEmpty() const override { return false; }
    [[nodiscard]] QString translate(const char* context,
                                    const char* sourceText,
                                    const char* disambiguation = nullptr,
                                    int n = -1) const override;

private:
    QString m_langCode = QStringLiteral("uk");
    QHash<QString, QHash<QString, QString>> m_dicts;
};

} // namespace gofi
