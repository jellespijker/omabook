#include "mcparguments.h"

#include <QJsonArray>
#include <QStringList>
#include <limits>

namespace {

QString typeOf(const QJsonValue &value) {
    switch (value.type()) {
    case QJsonValue::Bool:
        return "boolean";
    case QJsonValue::Double:
        return "number";
    case QJsonValue::String:
        return "string";
    case QJsonValue::Array:
        return "array";
    case QJsonValue::Object:
        return "object";
    default:
        return "null";
    }
}

// The type the schema names, and one of its values where it lists them.
QString checkKind(const QJsonObject &schema, const QJsonValue &value, const QString &name) {
    const QString type = schema.value("type").toString();
    const bool whole = value.isDouble() && value.toDouble() == static_cast<double>(value.toInteger());
    const bool fits = type.isEmpty() || (type == "integer" ? whole : typeOf(value) == type);
    if (!fits)
        return QString("\"%1\" must be %2, not %3.").arg(name, type == "integer" ? "a whole number" : "of type " + type, typeOf(value));
    if (schema.contains("enum") && !schema.value("enum").toArray().contains(value)) {
        QStringList allowed;
        for (const auto &option : schema.value("enum").toArray())
            allowed << "\"" + option.toString() + "\"";
        return QString("\"%1\" must be one of %2.").arg(name, allowed.join(", "));
    }
    return {};
}

// A number within the schema's bounds, and a whole one within what a tool reads.
QString checkRange(const QJsonObject &schema, const QJsonValue &value, const QString &name) {
    if (!value.isDouble())
        return {};
    if (schema.contains("minimum") && value.toDouble() < schema.value("minimum").toDouble())
        return QString("\"%1\" must be at least %2.").arg(name).arg(schema.value("minimum").toDouble());
    if (schema.contains("maximum") && value.toDouble() > schema.value("maximum").toDouble())
        return QString("\"%1\" must be at most %2.").arg(name).arg(schema.value("maximum").toDouble());
    // A whole number past an int would be read as the tool's default (#230).
    constexpr auto most = std::numeric_limits<int>::max();
    if (schema.value("type").toString() == "integer" && value.toDouble() > most)
        return QString("\"%1\" must be at most %2.").arg(name).arg(most);
    return {};
}

QString checkItems(const QJsonObject &schema, const QJsonValue &value, const QString &name) {
    if (!value.isArray() || !schema.contains("items"))
        return {};
    const QJsonArray items = value.toArray();
    for (qsizetype i = 0; i < items.size(); ++i)
        if (const QString problem = McpArguments::check(schema.value("items").toObject(), items.at(i), QString("%1[%2]").arg(name).arg(i));
            !problem.isEmpty())
            return problem;
    return {};
}

QString checkMembers(const QJsonObject &schema, const QJsonValue &value, const QString &name) {
    if (!value.isObject() || !schema.contains("properties"))
        return {};
    const QJsonObject object = value.toObject();
    const QJsonObject properties = schema.value("properties").toObject();
    for (const auto &required : schema.value("required").toArray())
        if (!object.contains(required.toString()))
            return QString("\"%1\" is required.").arg(name.isEmpty() ? required.toString() : name + "." + required.toString());
    for (auto member = object.begin(); member != object.end(); ++member) {
        const QString path = name.isEmpty() ? member.key() : name + "." + member.key();
        if (!properties.contains(member.key())) {
            if (schema.value("additionalProperties").toBool(true))
                continue;
            return QString("There is no argument \"%1\"; the arguments are: %2.").arg(path, properties.keys().join(", "));
        }
        if (const QString problem = McpArguments::check(properties.value(member.key()).toObject(), member.value(), path);
            !problem.isEmpty())
            return problem;
    }
    return {};
}

} // namespace

QString McpArguments::check(const QJsonObject &schema, const QJsonValue &value, const QString &name) {
    for (const auto &rule : {checkKind, checkRange, checkItems, checkMembers})
        if (const QString problem = rule(schema, value, name); !problem.isEmpty())
            return problem;
    return {};
}

QJsonObject McpArguments::atMost(QJsonObject schema, int most) {
    schema.insert("maximum", most);
    return schema;
}
