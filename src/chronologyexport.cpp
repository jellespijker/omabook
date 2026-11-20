#include "chronologyexport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>

#include "chronology.h"
#include "graph.h"

using namespace Qt::StringLiterals;

namespace {

// A lane's timeline as a node: a book's own order, which is no thing of the
// graph, stands as the book's number below zero.
[[nodiscard]] GraphNode timelineOf(const QJsonObject &lane, const QJsonObject &books) {
    const QJsonObject line = lane.value("timeline").toObject();
    GraphNode node;
    node.kind = Chronology::timelineKind();
    node.id = line.value("id").toInteger();
    node.name = line.value("name").toString();
    if (node.id == 0) {
        const qint64 book = line.value("book").toInteger();
        node.id = -book;
        node.name = books.value(QString::number(book)).toString(u"book %1"_s.arg(book)) + u"'s own order"_s;
    }
    node.summary = line.value("whose").toString();
    return node;
}

// The events and timelines of every lane, with the order between them.
class Picture {
public:
    explicit Picture(const QJsonObject &answer) {
        const QJsonObject books = answer.value("books").toObject();
        for (const auto &part : answer.value("parts").toArray())
            for (const auto &lane : part.toObject().value("lanes").toArray())
                add(lane.toObject(), books);
    }

    [[nodiscard]] GraphView view() const { return {.nodes = m_nodes.values(), .edges = m_edges}; }

private:
    void add(const QJsonObject &lane, const QJsonObject &books) {
        const GraphNode timeline = timelineOf(lane, books);
        m_nodes.insert(timeline.id, timeline);
        const QString calendar = lane.value("calendar").toString();
        QJsonObject previous;
        for (const auto &value : lane.value("items").toArray()) {
            const QJsonObject item = value.toObject();
            GraphNode event;
            event.id = item.value("event").toInteger();
            event.kind = u"event"_s;
            event.name = item.value("name").toString();
            event.summary = ChronologyExport::said(item.value("time").toObject(), calendar);
            m_nodes.insert(event.id, event);
            relate(event.id, timeline.id, Chronology::onRelation());
            if (const qint64 within = item.value("within").toInteger(); within != 0)
                relate(event.id, within, u"during"_s);
            if (!previous.isEmpty())
                relate(previous.value("event").toInteger(), event.id,
                       previous.value("rank").toInt() == item.value("rank").toInt() ? u"same time as"_s : u"before"_s);
            previous = item;
        }
    }
    void relate(qint64 source, qint64 target, const QString &kind) {
        const QString key = u"%1 %2 %3"_s.arg(source).arg(target).arg(kind);
        if (m_related.contains(key))
            return;
        m_related.insert(key);
        m_edges.append(
            {.id = m_edges.size() + 1, .source = source, .target = target, .kind = kind, .note = {}, .learned = {}, .anchor = {}});
    }

    QMap<qint64, GraphNode> m_nodes;
    QList<GraphEdge> m_edges;
    QSet<QString> m_related;
};

[[nodiscard]] QString csvField(const QString &text) {
    if (!text.contains(u',') && !text.contains(u'"') && !text.contains(u'\n'))
        return text;
    return u"\""_s + QString(text).replace(u"\""_s, u"\"\""_s) + u"\""_s;
}

[[nodiscard]] QByteArray csv(const QJsonObject &answer) {
    const QJsonObject books = answer.value("books").toObject();
    QStringList rows{u"timeline,timeline_id,calendar,rank,event,event_id,time,extent_from,extent_to,book,section,fraction,by_hand"_s};
    for (const auto &part : answer.value("parts").toArray())
        for (const auto &laneValue : part.toObject().value("lanes").toArray()) {
            const QJsonObject lane = laneValue.toObject();
            const GraphNode timeline = timelineOf(lane, books);
            const QString calendar = lane.value("calendar").toString();
            for (const auto &value : lane.value("items").toArray()) {
                const QJsonObject item = value.toObject();
                const QJsonObject revealed = item.value("revealed").toObject();
                const qint64 book = revealed.value("book").toInteger();
                const auto number = [&item](const char *key) {
                    return item.contains(QLatin1StringView(key)) ? QString::number(item.value(QLatin1StringView(key)).toInteger())
                                                                 : QString();
                };
                rows << QStringList{csvField(timeline.name),
                                    QString::number(timeline.id),
                                    csvField(calendar),
                                    QString::number(item.value("rank").toInt() + 1),
                                    csvField(item.value("name").toString()),
                                    QString::number(item.value("event").toInteger()),
                                    csvField(ChronologyExport::said(item.value("time").toObject(), calendar)),
                                    number("from"),
                                    number("to"),
                                    book > 0 ? csvField(books.value(QString::number(book)).toString()) : QString(),
                                    revealed.isEmpty() ? QString() : QString::number(revealed.value("section").toInt()),
                                    revealed.isEmpty() ? QString() : QString::number(revealed.value("fraction").toDouble()),
                                    item.value("by_hand").toBool() ? u"yes"_s : QString()}
                            .join(u',');
            }
        }
    return (rows.join(u'\n') + u'\n').toUtf8();
}

} // namespace

QStringList ChronologyExport::formats() { return {u"json"_s, u"csv"_s, u"graphml"_s, u"dot"_s}; }

std::expected<QByteArray, QString> ChronologyExport::exported(const QJsonObject &answer, const QString &format) {
    if (format == "json"_L1)
        return QJsonDocument(answer).toJson(QJsonDocument::Indented);
    if (format == "csv"_L1)
        return csv(answer);
    if (format == "graphml"_L1 || format == "dot"_L1)
        return Picture(answer).view().exported(format);
    return std::unexpected(u"\"%1\" is no format of the chronology: %2."_s.arg(format, formats().join(u", "_s)));
}

QString ChronologyExport::said(const QJsonObject &time, const QString &calendar) {
    QString when = time.value("at").toString();
    if (when.isEmpty() && (time.contains("from") || time.contains("to")))
        when = time.value("from").toString(u"…"_s) + u" – "_s + time.value("to").toString(u"…"_s);
    if (!when.isEmpty() && time.value("approximate").toBool())
        when = u"about "_s + when;
    if (const QString on = time.value("calendar").toString(); !when.isEmpty() && on != calendar)
        when += u" ("_s + on + u")"_s;
    if (const QString words = time.value("said").toString(); !words.isEmpty())
        when += (when.isEmpty() ? QString() : u" "_s) + u"“"_s + words + u"”"_s;
    return when;
}
