#include "scriptform.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace ScriptForm {

namespace {

const QStringList types{"text", "textarea", "choice", "multi", "number", "toggle"};

bool chooses(const QString &type) { return type == "choice" || type == "multi"; }

QStringList choiceValues(const QJsonObject &field) {
    QStringList values;
    for (const QJsonValue &choice : field.value("choices").toArray())
        values << choice.toObject().value("value").toString();
    return values;
}

// A value given as a number or as words, as words: a choice is told by them.
QString word(const QJsonValue &value) {
    if (value.isDouble())
        return QString::number(value.toDouble());
    if (value.isBool())
        return value.toBool() ? "true" : "false";
    return value.toString();
}

// A number kept inside the field's bounds and on its steps.
double fitted(const QJsonObject &field, double number) {
    if (!std::isfinite(number))
        number = field.value("value").toDouble();
    const double step = field.value("step").toDouble(1);
    if (step > 0) {
        const double from = field.contains("min") ? field.value("min").toDouble() : 0;
        number = from + std::round((number - from) / step) * step;
    }
    if (field.contains("min"))
        number = std::max(number, field.value("min").toDouble());
    if (field.contains("max"))
        number = std::min(number, field.value("max").toDouble());
    return number;
}

QJsonValue fit(const QJsonObject &field, const QJsonValue &given, bool *fits) {
    const QString type = field.value("type").toString();
    *fits = true;
    if (type == "text" || type == "textarea") {
        if (given.isString())
            return type == "text" ? given.toString().trimmed() : given.toString();
        if (given.isDouble())
            return word(given);
    } else if (type == "toggle") {
        if (given.isBool())
            return given;
    } else if (type == "number") {
        if (given.isDouble())
            return fitted(field, given.toDouble());
        bool number = false;
        const double parsed = given.toString().trimmed().toDouble(&number);
        if (number)
            return fitted(field, parsed);
    } else if (type == "choice") {
        if (choiceValues(field).contains(word(given)))
            return word(given);
    } else if (type == "multi" && given.isArray()) {
        // In the order the choices are offered, each once, and only those there are.
        const QStringList offered = choiceValues(field);
        QSet<QString> picked;
        for (const QJsonValue &one : given.toArray())
            picked.insert(word(one));
        QJsonArray kept;
        for (const QString &value : offered)
            if (picked.contains(value))
                kept << value;
        return kept;
    }
    *fits = false;
    return {};
}

} // namespace

QJsonObject checked(const QJsonObject &form, QString *error) {
    auto fail = [error](const QString &why) {
        *error = why;
        return QJsonObject();
    };
    const QString title = form.value("title").toString().simplified();
    if (title.isEmpty())
        return fail("A form needs a title: what it is for, as in \"Start a story\".");
    // No fields at all is a form too: a question to confirm or leave.
    const QJsonValue fields = form.contains("fields") ? form.value("fields") : QJsonValue(QJsonArray());
    if (!fields.isArray())
        return fail("A form's fields should be a list of fields, as in [{id: \"wish\", label: \"What should it be?\"}].");
    if (fields.toArray().size() > mostFields)
        return fail(QString("A form has at most %1 fields; this one has %2.").arg(mostFields).arg(fields.toArray().size()));

    static const QRegularExpression name("^[A-Za-z_][A-Za-z0-9_-]{0,63}$");
    QJsonArray made;
    QSet<QString> ids;
    for (const QJsonValue &value : fields.toArray()) {
        if (!value.isObject())
            return fail("Each of a form's fields should be an object: {id, label, type, ...}.");
        const QJsonObject given = value.toObject();
        const QString id = given.value("id").toString();
        if (!name.match(id).hasMatch())
            return fail("A field's id is a letter, then letters, digits, - and _; this one is \"" + id + "\".");
        if (ids.contains(id))
            return fail("Two fields are called \"" + id + "\"; each needs an id of its own.");
        ids.insert(id);
        const QString type = given.value("type").toString("text");
        if (!types.contains(type))
            return fail("The field \"" + id + "\" is of the type \"" + type + "\"; a field is one of: " + types.join(", ") + ".");

        QJsonObject field{{"id", id}, {"type", type}, {"label", given.value("label").toString(id).simplified()},
                          {"help", given.value("help").toString().simplified()},
                          {"placeholder", given.value("placeholder").toString().simplified()},
                          {"required", given.value("required").toBool(false)}};
        if (chooses(type)) {
            const QJsonArray choices = given.value("choices").toArray();
            if (choices.isEmpty())
                return fail("The field \"" + id + "\" is a " + type + " and has no choices to offer.");
            if (choices.size() > mostChoices)
                return fail(QString("The field \"%1\" offers %2 choices; a field offers at most %3.").arg(id).arg(choices.size()).arg(mostChoices));
            QJsonArray tidy;
            QSet<QString> values;
            for (const QJsonValue &choice : choices) {
                const QJsonObject object = choice.isObject() ? choice.toObject() : QJsonObject{{"value", choice}};
                const QString of = word(object.value("value"));
                if (of.isEmpty())
                    return fail("A choice of \"" + id + "\" has no value; a choice is a string, or {value, label, help}.");
                if (values.contains(of))
                    return fail("The field \"" + id + "\" offers \"" + of + "\" twice.");
                values.insert(of);
                tidy << QJsonObject{{"value", of}, {"label", object.value("label").toString(of).simplified()},
                                    {"help", object.value("help").toString().simplified()}};
            }
            field.insert("choices", tidy);
        }
        if (type == "number") {
            for (const char *bound : {"min", "max", "step"}) {
                if (!given.contains(bound))
                    continue;
                if (!given.value(bound).isDouble() || !std::isfinite(given.value(bound).toDouble()))
                    return fail(QString("The field \"%1\"'s %2 should be a number.").arg(id, bound));
                field.insert(bound, given.value(bound).toDouble());
            }
            if (field.contains("min") && field.contains("max") && field.value("min").toDouble() > field.value("max").toDouble())
                return fail("The field \"" + id + "\"'s min is above its max.");
            if (field.contains("step") && field.value("step").toDouble() <= 0)
                return fail("The field \"" + id + "\"'s step should be above 0.");
        }
        // Where it starts: what was given when it fits, else the first choice,
        // the lowest number, nothing ticked, no words.
        const QJsonValue start = type == "choice"   ? QJsonValue(field.value("choices").toArray().first().toObject().value("value"))
                                 : type == "multi"  ? QJsonValue(QJsonArray())
                                 : type == "number" ? QJsonValue(field.contains("min") ? field.value("min").toDouble() : 0.0)
                                 : type == "toggle" ? QJsonValue(false)
                                                    : QJsonValue(QString());
        field.insert("value", start);
        if (given.contains("value")) {
            bool fits = false;
            const QJsonValue value = fit(field, given.value("value"), &fits);
            if (!fits)
                return fail("The field \"" + id + "\" starts at a value that isn't one it can have.");
            field.insert("value", value);
        }
        if (given.contains("when")) {
            if (!given.value("when").isObject())
                return fail("The field \"" + id + "\"'s when should be an object, as in {\"where\": [\"sequel\", \"branch\"]}.");
            field.insert("when", given.value("when"));
        }
        made << field;
    }
    // A field is shown by the fields above it, so the form reads downwards.
    for (int i = 0; i < made.size(); ++i) {
        const QJsonObject when = made.at(i).toObject().value("when").toObject();
        for (auto it = when.begin(); it != when.end(); ++it) {
            bool above = false;
            for (int j = 0; j < i; ++j)
                above = above || made.at(j).toObject().value("id").toString() == it.key();
            if (!above)
                return fail("The field \"" + made.at(i).toObject().value("id").toString() + "\" is shown by \"" + it.key()
                            + "\", which is not a field above it.");
        }
    }
    return {{"title", title}, {"intro", form.value("intro").toString().trimmed()},
            {"accept", form.value("accept").toString().simplified()}, {"fields", made}};
}

QJsonObject values(const QJsonObject &form, const QJsonObject &given) {
    QJsonObject values;
    for (const QJsonValue &value : form.value("fields").toArray()) {
        const QJsonObject field = value.toObject();
        const QString id = field.value("id").toString();
        bool fits = false;
        const QJsonValue answer = given.contains(id) ? fit(field, given.value(id), &fits) : QJsonValue();
        values.insert(id, fits ? answer : field.value("value"));
    }
    return values;
}

bool shown(const QJsonObject &field, const QJsonObject &values) {
    const QJsonObject when = field.value("when").toObject();
    for (auto it = when.begin(); it != when.end(); ++it) {
        const QJsonValue now = values.value(it.key());
        const QJsonArray wanted = it.value().isArray() ? it.value().toArray() : QJsonArray{it.value()};
        // A list that is ticked shows the field when any one wanted is.
        const QJsonArray have = now.isArray() ? now.toArray() : QJsonArray{now};
        bool any = false;
        for (const QJsonValue &one : wanted)
            for (const QJsonValue &has : have)
                any = any || word(has) == word(one);
        if (!any)
            return false;
    }
    return true;
}

QString missing(const QJsonObject &form, const QJsonObject &values) {
    for (const QJsonValue &value : form.value("fields").toArray()) {
        const QJsonObject field = value.toObject();
        if (!field.value("required").toBool() || !shown(field, values))
            continue;
        const QJsonValue has = values.value(field.value("id").toString());
        if ((has.isString() && has.toString().trimmed().isEmpty()) || (has.isArray() && has.toArray().isEmpty()))
            return field.value("label").toString();
    }
    return {};
}

} // namespace ScriptForm
