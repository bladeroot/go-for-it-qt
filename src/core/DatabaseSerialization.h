#pragma once

#include "Schedule.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace gofi {

inline QString nonNullString(const QString& s) {
    return s.isNull() ? QStringLiteral("") : s;
}

inline QString serializeSchedule(const Schedule& schedule) {
    QJsonArray arr;
    for (const QVariant& v : schedule.toVariantList()) {
        arr.append(v.toInt());
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

inline Schedule deserializeSchedule(const QString& json) {
    Schedule sched;
    if (json.isEmpty()) {
        return sched;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isArray()) {
        sched.fromVariantList(doc.array().toVariantList());
    }
    return sched;
}

inline QString serializeStatusMapping(const QVariantMap& map) {
    if (map.isEmpty()) return QStringLiteral("");
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

inline QVariantMap deserializeStatusMapping(const QString& json) {
    if (json.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isObject()) {
        return doc.object().toVariantMap();
    }
    return {};
}

inline QString serializePriorityMapping(const QVariantMap& map) {
    if (map.isEmpty()) return QStringLiteral("");
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

inline QVariantMap deserializePriorityMapping(const QString& json) {
    if (json.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isObject()) {
        return doc.object().toVariantMap();
    }
    return {};
}

inline QString serializeCommentsList(const QVariantList& comments) {
    if (comments.isEmpty()) return QStringLiteral("");
    return QString::fromUtf8(QJsonDocument(QJsonArray::fromVariantList(comments)).toJson(QJsonDocument::Compact));
}

inline QVariantList deserializeCommentsList(const QString& json) {
    if (json.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isArray()) {
        return doc.array().toVariantList();
    }
    return {};
}

} // namespace gofi
