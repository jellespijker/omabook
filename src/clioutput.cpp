#include "clioutput.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <print>
#include <tuple>

void CliOutput::print(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

int CliOutput::fail(const QString &why) {
    print(stderr, why);
    return 1;
}

void CliOutput::printJson(const QJsonValue &value) {
    const QJsonDocument document = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
    print(stdout, QString::fromUtf8(document.toJson(QJsonDocument::Indented)).trimmed());
}
