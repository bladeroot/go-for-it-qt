#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QTranslator>
#include <QVariantList>

namespace gofi {

class UkTranslator : public QTranslator {
    Q_OBJECT
public:
    explicit UkTranslator(QObject* parent = nullptr);

    void setLanguageCode(const QString& code);
    [[nodiscard]] QString languageCode() const { return m_langCode; }

    void reloadTranslations();
    bool loadTranslationsFromJson(const QString& filePath, const QString& langCode);
    void loadTranslationsFromDirectory(const QString& dirPath);

    [[nodiscard]] QStringList availableLanguageCodes() const;
    [[nodiscard]] QVariantList availableLanguagesModel() const;
    [[nodiscard]] QString languageDisplayName(const QString& code) const;
    [[nodiscard]] QString resolveEffectiveLanguage(const QString& requestedCode) const;
    [[nodiscard]] bool hasLanguage(const QString& code) const;

    [[nodiscard]] bool isEmpty() const override { return false; }
    [[nodiscard]] QString translate(const char* context,
                                    const char* sourceText,
                                    const char* disambiguation = nullptr,
                                    int n = -1) const override;

private:
    QString m_langCode = QStringLiteral("uk");
    QStringList m_availableLanguages;
    QHash<QString, QHash<QString, QString>> m_dicts;
};

} // namespace gofi
