#include "pluginargument.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

std::expected<QJsonValue, QString> PluginArgument::parse(const QStringList &words, FILE *input) {
    if (words.isEmpty())
        return QJsonValue(QJsonValue::Undefined);
    QByteArray text;
    if (words == QStringList{QStringLiteral("-")}) {
        QFile in;
        if (in.open(input, QIODevice::ReadOnly))
            text = in.readAll();
    } else {
        text = words.join(u' ').toUtf8();
    }
    QJsonParseError error{};
    // A document is an object or a list, so a bare value rides in a list.
    const QJsonDocument document = QJsonDocument::fromJson('[' + text + ']', &error);
    if (error.error != QJsonParseError::NoError)
        return std::unexpected(error.errorString());
    return document.array().at(0);
}
