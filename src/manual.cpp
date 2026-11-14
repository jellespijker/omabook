#include "manual.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QRegularExpression>
#include <QVersionNumber>

#include "customization.h"
#include "settings.h"

namespace {

// "{library.add}", and what a plugin's command is called: "{library.shelver:shelve}".
[[nodiscard]] const QRegularExpression &braces() {
    static const QRegularExpression braces("\\{([a-z]+\\.[A-Za-z0-9_:.-]+)\\}");
    return braces;
}

[[nodiscard]] QString placeOf(const QString &command) { return command.section('.', 0, 0); }

[[nodiscard]] QVersionNumber number(const QString &version) { return QVersionNumber::fromString(version); }

// A terminal line no wider than most terminals are.
[[nodiscard]] QString wrapped(const QString &text, int width = 78) {
    QStringList lines;
    QString line;
    for (const QString &word : text.split(' ', Qt::SkipEmptyParts)) {
        if (!line.isEmpty() && line.size() + 1 + word.size() > width) {
            lines << line;
            line.clear();
        }
        line += (line.isEmpty() ? "" : " ") + word;
    }
    if (!line.isEmpty())
        lines << line;
    return lines.join('\n');
}

} // namespace

Manual::Manual(Settings *settings, Customization *custom, const QString &version, QObject *parent)
    : QObject(parent), m_settings(settings), m_custom(custom), m_version(version.isEmpty() ? QString(OMABOOK_VERSION) : version) {
    connect(custom, &Customization::keysChanged, this, &Manual::changed);
    if (!settings)
        return;
    connect(settings, &Settings::changed, this, &Manual::changed);
    // A release that brought nothing worth a note is taken as seen, so the
    // next one that does is measured from here.
    if (!settings->tour().isEmpty() && number(m_version) > number(settings->newsSeen()) && news().isEmpty())
        settings->setNewsSeen(m_version);
}

QStringList Manual::mentioned(const QString &text) {
    QStringList commands;
    for (auto it = braces().globalMatch(text); it.hasNext();) {
        const QString command = it.next().captured(1);
        if (!commands.contains(command))
            commands << command;
    }
    return commands;
}

QString Manual::firstKey(const QString &command) const {
    const QString key = m_custom->keyText().value(command).toString().section("  ", 0, 0);
    return key.isEmpty() ? tr("no key") : key;
}

QString Manual::plain(const QString &text) const {
    QString result = text;
    for (const QString &command : mentioned(text))
        result.replace("{" + command + "}", firstKey(command));
    return result;
}

QString Manual::html(const QString &text) const {
    QString result = text.toHtmlEscaped();
    for (const QString &command : mentioned(text))
        result.replace("{" + command + "}", "<b>" + firstKey(command).toHtmlEscaped() + "</b>");
    static const QRegularExpression code("`([^`]+)`");
    result.replace(code, "<code>\\1</code>");
    return result;
}

QVariantList Manual::commands() const {
    QVariantList list;
    const QVariantMap keys = m_custom->keyText();
    for (const Customization::Command &command : Customization::commands()) {
        const QString all = keys.value(command.id).toString();
        // The table is in English, for the guide; here it is shown.
        list << QVariantMap{{"id", command.id}, {"place", placeOf(command.id)}, {"keys", all}, {"key", all.section("  ", 0, 0)},
                            {"text", QCoreApplication::translate("Customization", command.what.toUtf8().constData())}};
    }
    for (const QVariant &value : m_custom->pluginCommands()) {
        const QVariantMap plugin = value.toMap();
        const QString id = plugin.value("id").toString();
        const QString all = keys.value(id).toString();
        list << QVariantMap{{"id", id}, {"place", "plugins"}, {"keys", all}, {"key", all.section("  ", 0, 0)},
                            {"text", plugin.value("title").toString()}};
    }
    return list;
}

QVariantList Manual::topics() const {
    QVariantList list;
    for (const Topic &topic : topicList()) {
        QVariantList sections;
        QStringList named;
        QString words = topic.title + " " + topic.summary;
        for (const Section &section : topic.sections) {
            sections << QVariantMap{{"heading", section.heading}, {"html", html(section.text)}};
            for (const QString &command : mentioned(section.text))
                if (!named.contains(command))
                    named << command;
            words += " " + section.heading + " " + plain(section.text);
        }
        list << QVariantMap{{"id", topic.id}, {"title", topic.title}, {"summary", topic.summary},
                            {"sections", sections}, {"commands", named}, {"search", words.toLower()}};
    }
    return list;
}

QVariantMap Manual::card(const Step &step) const {
    QString what;
    for (const QVariant &value : commands())
        if (value.toMap().value("id").toString() == step.command)
            what = value.toMap().value("text").toString();
    return {{"id", step.id}, {"title", step.title}, {"html", html(step.text)}, {"text", plain(step.text)}, {"spot", step.spot},
            {"command", step.command}, {"key", step.command.isEmpty() ? QString() : firstKey(step.command)},
            {"what", what}, {"place", step.place}};
}

QVariantList Manual::tour(const QString &place, bool empty, bool touch) const {
    QVariantList cards;
    for (const Step &step : tourSteps()) {
        const bool fits = step.when.isEmpty() || (step.when == "empty" && empty) || (step.when == "books" && !empty)
                          || (step.when == "touch" && touch);
        if (fits && (step.place.isEmpty() || step.place == place))
            cards << card(step);
    }
    return cards;
}

QVariantList Manual::news(bool all) const {
    const QVersionNumber current = number(m_version);
    const QVersionNumber seen = number(m_settings ? m_settings->newsSeen() : QString());
    // "Everything the newest release brought" is the newest this build
    // knows of, which is this version unless it added nothing.
    QVersionNumber newest;
    for (const Step &feature : features())
        if (number(feature.version) <= current)
            newest = std::max(newest, number(feature.version));
    QVariantList cards;
    for (const Step &feature : features()) {
        const QVersionNumber arrived = number(feature.version);
        if (all ? arrived == newest : (arrived > seen && arrived <= current))
            cards << card(feature);
    }
    return cards;
}

bool Manual::tourOffered() const { return m_settings && m_settings->tour().isEmpty(); }

bool Manual::readerTourOffered() const { return m_settings && m_settings->tour() == "library"; }

bool Manual::newsOffered() const {
    return m_settings && !m_settings->tour().isEmpty() && !m_settings->newsSeen().isEmpty()
           && number(m_version) > number(m_settings->newsSeen()) && !news().isEmpty();
}

// The state only ever moves forward: never offered, the shelf's half taken
// (so the reader's half is offered in the first book), done. Taking the tour
// again from the help changes nothing, so it can't come back uninvited.
void Manual::tourAnswered(const QString &place, bool finished) {
    if (!m_settings)
        return;
    const QString state = m_settings->tour();
    if (state.isEmpty()) {
        // Whoever starts with this version has nothing that is new to them.
        m_settings->setNewsSeen(m_version);
        m_settings->setTour(place == "library" && finished ? "library" : "done");
    } else if (state == "library" && place == "reader") {
        m_settings->setTour("done");
    }
}

void Manual::newsAnswered() {
    if (m_settings)
        m_settings->setNewsSeen(m_version);
}

QString Manual::topicText(const QString &id) const {
    for (const Topic &topic : topicList()) {
        if (topic.id != id)
            continue;
        QString text = topic.title + "\n" + QString(topic.title.size(), '=') + "\n\n" + wrapped(topic.summary) + "\n";
        QStringList named;
        for (const Section &section : topic.sections) {
            text += "\n" + section.heading + "\n" + QString(section.heading.size(), '-') + "\n" + wrapped(plain(section.text)) + "\n";
            for (const QString &command : mentioned(section.text))
                if (!named.contains(command))
                    named << command;
        }
        if (!named.isEmpty()) {
            text += "\nKeys\n----\n";
            for (const QVariant &value : commands()) {
                const QVariantMap command = value.toMap();
                if (named.contains(command.value("id").toString()))
                    text += QString("  %1  %2\n").arg(command.value("keys").toString().leftJustified(18), command.value("text").toString());
            }
        }
        return text.trimmed();
    }
    return {};
}

QJsonObject Manual::topicJson(const QString &id) const {
    for (const Topic &topic : topicList()) {
        if (topic.id != id)
            continue;
        QJsonArray sections, keys;
        QStringList named;
        for (const Section &section : topic.sections) {
            sections << QJsonObject{{"heading", section.heading}, {"text", plain(section.text)}};
            named << mentioned(section.text);
        }
        for (const QVariant &value : commands()) {
            const QVariantMap command = value.toMap();
            if (named.contains(command.value("id").toString()))
                keys << QJsonObject{{"command", command.value("id").toString()}, {"keys", command.value("keys").toString()},
                                    {"what", command.value("text").toString()}};
        }
        return {{"id", topic.id}, {"title", topic.title}, {"summary", topic.summary}, {"sections", sections}, {"keys", keys}};
    }
    return {};
}

QString Manual::overviewText() const {
    QString text = "Help topics (omabook help <topic>):\n";
    for (const Topic &topic : topicList())
        text += QString("  %1  %2: %3\n").arg(topic.id.leftJustified(10), topic.title, topic.summary);
    text += QString("  %1  %2\n").arg(QString("keys").leftJustified(10), "Every key in the window, as it is bound now");
    text += QString("  %1  %2").arg(QString("new").leftJustified(10), "What this version brought");
    return text;
}

QJsonObject Manual::overviewJson() const {
    QJsonArray topics;
    for (const Topic &topic : topicList())
        topics << QJsonObject{{"id", topic.id}, {"title", topic.title}, {"summary", topic.summary}};
    topics << QJsonObject{{"id", "keys"}, {"title", "Keys"}, {"summary", "Every key in the window, as it is bound now"}};
    topics << QJsonObject{{"id", "new"}, {"title", "What's new"}, {"summary", "What this version brought"}};
    return {{"version", m_version}, {"topics", topics}};
}

QString Manual::keysText() const {
    QString text, place;
    const QHash<QString, QString> headings{{"global", "Everywhere"}, {"library", "On the shelf"}, {"reader", "While reading"}, {"graph", "In the graph"}, {"plugins", "Plugins"}};
    for (const QVariant &value : commands()) {
        const QVariantMap command = value.toMap();
        if (command.value("place").toString() != place) {
            place = command.value("place").toString();
            text += (text.isEmpty() ? "" : "\n") + headings.value(place) + "\n";
        }
        text += QString("  %1  %2\n").arg(command.value("keys").toString().leftJustified(18), command.value("text").toString());
    }
    return text.trimmed();
}

QJsonObject Manual::keysJson() const {
    QJsonArray keys;
    for (const QVariant &value : commands()) {
        const QVariantMap command = value.toMap();
        keys << QJsonObject{{"command", command.value("id").toString()}, {"place", command.value("place").toString()},
                            {"keys", command.value("keys").toString()}, {"what", command.value("text").toString()}};
    }
    return {{"keys", keys}};
}
