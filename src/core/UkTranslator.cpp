#include "UkTranslator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>

static void initTranslationsResource() {
    Q_INIT_RESOURCE(translations);
}

namespace gofi {

UkTranslator::UkTranslator(QObject* parent)
    : QTranslator(parent)
{
    reloadTranslations();
}

void UkTranslator::reloadTranslations() {
    initTranslationsResource();
    m_dicts.clear();
    m_availableLanguages.clear();

    // 1. Built-in Qt Resources (:/translations) — lowest priority baseline
    loadTranslationsFromDirectory(QStringLiteral(":/translations"));

    // 2. System data locations (/usr/share, /usr/local/share, etc.) in reverse order
    // standardLocations returns [~/.local/share, /usr/local/share, /usr/share]
    // Reversing ensures system /usr/share loads before local/user dirs
    QStringList sysDataDirs = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    std::reverse(sysDataDirs.begin(), sysDataDirs.end());
    const QString userGeneric = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    for (const QString& d : sysDataDirs) {
        if (!userGeneric.isEmpty() && d == userGeneric) continue;
        loadTranslationsFromDirectory(d + QStringLiteral("/go-for-it-qt/translations"));
    }

    // 3. Application installed / bundle share directory
    if (QCoreApplication::instance()) {
        const QString appDir = QCoreApplication::applicationDirPath();
        loadTranslationsFromDirectory(appDir + QStringLiteral("/../share/go-for-it-qt/translations"));
        // 4. Development build data directory (e.g. build/data/translations)
        loadTranslationsFromDirectory(appDir + QStringLiteral("/data/translations"));
    }

    // 5. User data location (~/.local/share/go-for-it-qt/translations) — overrides system & dev
    const QString appDataTranslations = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/translations");
    loadTranslationsFromDirectory(appDataTranslations);

    // 6. Environment variable override (highest priority)
    const QString customDir = QString::fromUtf8(qgetenv("GOFI_TRANSLATIONS_DIR"));
    if (!customDir.isEmpty()) {
        loadTranslationsFromDirectory(customDir);
    }
}

bool UkTranslator::loadTranslationsFromJson(const QString& filePath, const QString& langCode) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    const QJsonObject obj = doc.object();
    const QString code = langCode.trimmed().toLower();
    if (code.isEmpty()) return false;

    auto& targetDict = m_dicts[code];
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        targetDict[it.key()] = it.value().toString();
    }
    const QString baseCode = code.section(QLatin1Char('_'), 0, 0).section(QLatin1Char('-'), 0, 0);
    if (!m_availableLanguages.contains(baseCode) && baseCode != QStringLiteral("en")) {
        m_availableLanguages.append(baseCode);
    }
    return true;
}

void UkTranslator::loadTranslationsFromDirectory(const QString& dirPath) {
    if (dirPath.isEmpty()) return;
    QDir dir(dirPath);
    if (!dir.exists()) return;
    static const QRegularExpression langCodeRegex(QStringLiteral("^[a-z]{2,3}([_-][a-z]{2})?$"), QRegularExpression::CaseInsensitiveOption);
    const QStringList files = dir.entryList(QDir::Files);
    for (const QString& file : files) {
        if (!file.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) continue;
        const QString lang = file.left(file.lastIndexOf(QLatin1Char('.'))).toLower();
        if (!langCodeRegex.match(lang).hasMatch()) continue;
        loadTranslationsFromJson(dir.filePath(file), lang);
        if (lang.contains(QLatin1Char('_'))) {
            loadTranslationsFromJson(dir.filePath(file), lang.section(QLatin1Char('_'), 0, 0));
        } else if (lang.contains(QLatin1Char('-'))) {
            loadTranslationsFromJson(dir.filePath(file), lang.section(QLatin1Char('-'), 0, 0));
        }
    }
}

void UkTranslator::setLanguageCode(const QString& code) {
    m_langCode = code.trimmed().toLower();
}

bool UkTranslator::hasLanguage(const QString& code) const {
    const QString c = code.trimmed().toLower();
    if (c.isEmpty()) return false;
    if (m_dicts.contains(c) && !m_dicts.value(c).isEmpty()) return true;
    if (c.contains(QLatin1Char('_')) || c.contains(QLatin1Char('-'))) {
        const QString base = c.section(QLatin1Char('_'), 0, 0).section(QLatin1Char('-'), 0, 0);
        return m_dicts.contains(base) && !m_dicts.value(base).isEmpty();
    }
    return false;
}

QString UkTranslator::languageDisplayName(const QString& code) const {
    const QString c = code.trimmed().toLower();
    if (c == QStringLiteral("system")) {
        return tr("System (Auto)");
    }
    if (c == QStringLiteral("en")) {
        return QStringLiteral("English");
    }
    QLocale loc(c);
    QString native = loc.nativeLanguageName();
    if (!native.isEmpty()) {
        native[0] = native[0].toUpper();
    }
    const QString eng = QLocale::languageToString(loc.language());
    if (!native.isEmpty() && !eng.isEmpty() && native.compare(eng, Qt::CaseInsensitive) != 0 && eng != QStringLiteral("C")) {
        return QStringLiteral("%1 (%2)").arg(native, eng);
    }
    if (!native.isEmpty()) {
        return native;
    }
    if (!eng.isEmpty() && eng != QStringLiteral("C")) {
        return eng;
    }
    return c;
}

QVariantList UkTranslator::availableLanguagesModel() const {
    QVariantList list;

    // 1. System (Auto)
    QVariantMap sysItem;
    sysItem[QStringLiteral("id")] = QStringLiteral("system");
    sysItem[QStringLiteral("label")] = tr("System (Auto)");
    list.append(sysItem);

    // 2. Ukrainian (prioritize uk first if present)
    QStringList codes = m_availableLanguages;
    codes.sort();
    if (codes.contains(QStringLiteral("uk"))) {
        codes.removeAll(QStringLiteral("uk"));
        codes.prepend(QStringLiteral("uk"));
    }

    for (const QString& code : codes) {
        if (code == QStringLiteral("en")) continue;
        QVariantMap item;
        item[QStringLiteral("id")] = code;
        item[QStringLiteral("label")] = languageDisplayName(code);
        list.append(item);
    }

    // 3. English (base default)
    QVariantMap enItem;
    enItem[QStringLiteral("id")] = QStringLiteral("en");
    enItem[QStringLiteral("label")] = QStringLiteral("English");
    list.append(enItem);

    return list;
}

QStringList UkTranslator::availableLanguageCodes() const {
    QStringList list;
    list.append(QStringLiteral("system"));
    QStringList codes = m_availableLanguages;
    codes.sort();
    if (codes.contains(QStringLiteral("uk"))) {
        codes.removeAll(QStringLiteral("uk"));
        codes.prepend(QStringLiteral("uk"));
    }
    for (const QString& code : codes) {
        if (code != QStringLiteral("en") && !list.contains(code)) {
            list.append(code);
        }
    }
    if (!list.contains(QStringLiteral("en"))) {
        list.append(QStringLiteral("en"));
    }
    return list;
}

QString UkTranslator::resolveEffectiveLanguage(const QString& requestedCode) const {
    const QString norm = requestedCode.trimmed().toLower();
    if (norm == QStringLiteral("en")) {
        return QStringLiteral("en");
    }

    if (!norm.isEmpty() && norm != QStringLiteral("system")) {
        if (hasLanguage(norm)) return norm;
        if (norm.contains(QLatin1Char('_')) || norm.contains(QLatin1Char('-'))) {
            const QString base = norm.section(QLatin1Char('_'), 0, 0).section(QLatin1Char('-'), 0, 0);
            if (hasLanguage(base)) return base;
        }
        return norm;
    }

    // System resolution
    const QString sysName = QLocale::system().name().toLower();
    if (hasLanguage(sysName)) return sysName;
    const QString sysBase = sysName.section(QLatin1Char('_'), 0, 0).section(QLatin1Char('-'), 0, 0);
    if (hasLanguage(sysBase)) return sysBase;

    for (const QString& uiLang : QLocale::system().uiLanguages()) {
        const QString uiLower = uiLang.toLower();
        if (hasLanguage(uiLower)) return uiLower;
        const QString uiBase = uiLower.section(QLatin1Char('_'), 0, 0).section(QLatin1Char('-'), 0, 0);
        if (hasLanguage(uiBase)) return uiBase;
    }

    return QStringLiteral("en");
}

QString UkTranslator::translate(const char* context,
                                const char* sourceText,
                                const char* disambiguation,
                                int n) const
{
    Q_UNUSED(context)
    Q_UNUSED(disambiguation)
    Q_UNUSED(n)
    if (!sourceText) return {};
    const QString key = QString::fromUtf8(sourceText);

    auto tryLookup = [this, &key](const QString& code) -> QString {
        const auto dictIt = m_dicts.constFind(code);
        if (dictIt != m_dicts.constEnd()) {
            const auto it = dictIt->constFind(key);
            if (it != dictIt->constEnd()) return it.value();
        }
        return {};
    };

    QString res = tryLookup(m_langCode);
    if (!res.isEmpty()) return res;

    // Fallback: if m_langCode has region (e.g. "uk_ua" -> fallback to "uk")
    if (m_langCode.contains(QLatin1Char('_')) || m_langCode.contains(QLatin1Char('-'))) {
        const QString base = m_langCode.section(QLatin1Char('_'), 0, 0).section(QLatin1Char('-'), 0, 0);
        res = tryLookup(base);
        if (!res.isEmpty()) return res;
    }
    return {};
}

} // namespace gofi
