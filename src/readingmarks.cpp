#include "readingmarks.h"

#include "booksides.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <ranges>

#include "graphbuilder.h"
#include "graphcanvas.h"
#include "graphcontext.h"
#include "graphmodel.h"
#include "reader.h"

namespace {

// Where a mark keeps the thing it is of, in the characters' format.
constexpr int thingProperty = QTextFormat::UserProperty + 41;
// Kept on a chapter's document: which marks are on it, so a chapter marked
// already is left alone.
constexpr const char *stampProperty = "omabookMarks";
// Chapters whose marks are kept; a book is read a few at a time.
constexpr qsizetype kept = 48;

GraphPlace laterOf(const GraphPlace &a, const GraphPlace &b) { return a.notAfter(b) ? b : a; }

} // namespace

ReadingMarks::ReadingMarks(Reader *reader, QObject *parent) : QObject(parent), m_reader(reader) {
    connect(m_reader, &Reader::bookChanged, this, [this] {
        // A book read anew because it grew keeps what it knows; its text
        // is compared chapter by chapter anyway.
        if (!m_reader->growing())
            load();
    });
    connect(m_reader, &Reader::sectionChanged, this, &ReadingMarks::follow);
    connect(m_reader, &Reader::fractionChanged, this, &ReadingMarks::follow);
    connect(GraphFeed::instance(), &GraphFeed::changed, this, &ReadingMarks::refresh);
    // A moment's words stay lit long enough to be found on the page.
    m_flashOver = new QTimer(this);
    m_flashOver->setSingleShot(true);
    m_flashOver->setInterval(4000);
    connect(m_flashOver, &QTimer::timeout, this, [this] {
        m_flash.clear();
        emit flashChanged();
    });
    connect(m_reader, &Reader::bookChanged, this, [this] {
        if (m_flash.isEmpty() || m_reader->growing())
            return;
        m_flash.clear();
        emit flashChanged();
    });
}

ReadingMarks::~ReadingMarks() {
    // The work under way holds its own copy of the names and the text, and
    // is only waited for so that nothing is left running.
    for (auto &[key, watcher] : m_working)
        watcher->waitForFinished();
}

bool ReadingMarks::enabled() const { return !m_graphMode || m_graphMode() != "off"; }

void ReadingMarks::setColors(const QVariantMap &colors) {
    if (colors == m_colors)
        return;
    m_colors = colors;
    ++m_look;
    emit lookChanged();
}

void ReadingMarks::setForeground(const QColor &color) {
    if (color == m_foreground)
        return;
    m_foreground = color;
    ++m_look;
    emit lookChanged();
}

void ReadingMarks::setBackground(const QColor &color) {
    if (color == m_background)
        return;
    m_background = color;
    ++m_look;
    emit lookChanged();
}

QColor ReadingMarks::colorOf(const QString &kind) const { return GraphKinds::color(kind, m_colors, m_foreground, m_background); }

QColor ReadingMarks::tintOf(const QString &kind) const {
    // Light enough to read through in either theme: a light page needs a
    // little more of the color to show it at all.
    QColor tint = colorOf(kind);
    const bool lightPage = m_background.lightnessF() > 0.5;
    tint.setAlphaF(lightPage ? 0.16 : 0.13);
    return tint;
}

// Another book, or the graph changed: what the reader has met is read again.
void ReadingMarks::load() {
    const qint64 before = m_book;
    const Library *library = m_reader->isOpen() ? m_reader->library() : nullptr;
    m_book = library ? m_reader->bookId() : 0;
    m_graphCount = library ? library->changes().graph : -1;
    m_met = library && enabled() ? Graph(*library).metIn(m_book) : QList<std::pair<GraphNode, GraphPlace>>();
    m_facts = library && enabled()
                  ? Graph(*library).factsOf(m_met | std::views::transform([](const auto &met) { return met.first.id; })
                                            | std::ranges::to<QList<qint64>>())
                  : QHash<qint64, QList<GraphFact>>();
    // And the graphs someone shared, as far as the reader has read the book:
    // read again once they read on past that (follow).
    const QList<GraphLayers::Layer> layers = library && enabled() ? GraphContext::layers(*library) : QList<GraphLayers::Layer>();
    GraphLayers::addTo(m_met, m_facts, layers, m_book);
    m_layersTo = layers.isEmpty() ? std::nullopt : std::optional(laterOf(GraphContext::furthest(*library, m_book), m_reader->place()));
    m_factPlaces.clear();
    for (const QList<GraphFact> &facts : std::as_const(m_facts))
        for (const GraphFact &fact : facts)
            if (fact.revealed.place.bookId == m_book)
                m_factPlaces << fact.revealed.place;
    std::ranges::sort(m_factPlaces, [](const GraphPlace &a, const GraphPlace &b) { return a.notAfter(b) && !b.notAfter(a); });
    ++m_counts.loads;
    m_noted.clear();
    m_notedFor.clear();
    m_madeSuggestions.clear();
    rebuild(m_book != before);
}

void ReadingMarks::refresh() {
    const Library *library = m_reader->isOpen() ? m_reader->library() : nullptr;
    // The counter changes with any write to the graph, of any book; only
    // when this book's things changed is anything marked anew (rebuild
    // compares the names).
    if (!library || library->changes().graph == m_graphCount)
        return;
    load();
}

// The reader moved: only passing the place where something new was met
// changes what is marked.
void ReadingMarks::follow() {
    if (!m_reader->isOpen() || m_reader->bookId() != m_book)
        return;
    if (m_layersTo && !m_reader->place().notAfter(*m_layersTo)) {
        load();
        return;
    }
    if (TextMarks::reach(m_met, m_reader->place()) != m_reach || factReach(m_reader->place()) != m_factReach)
        rebuild(false);
}

qsizetype ReadingMarks::factReach(const GraphPlace &place) const {
    return std::ranges::count_if(m_factPlaces, [&place](const GraphPlace &at) { return at.notAfter(place); });
}

void ReadingMarks::rebuild(bool force) {
    const GraphPlace place = m_reader->isOpen() ? m_reader->place() : GraphPlace{};
    m_reach = TextMarks::reach(m_met, place);
    m_factReach = factReach(place);
    // Each thing as it was known here: a name revealed further on is not
    // marked, and its card says only what was known by now.
    QList<std::pair<GraphNode, GraphPlace>> met = m_met;
    // What a book on the other side of the library's top collections taught
    // is not this book's to say (booksides.h).
    const BookSides sides = m_reader->isOpen() && m_reader->library() ? BookSides::of(*m_reader->library()) : BookSides();
    const GraphKnown known = [book = m_book, place, &sides](const GraphPlace &at) {
        return at.bookId != book ? sides.together(at.bookId, book) : at.notAfter(place);
    };
    for (auto &[node, at] : met)
        node = Graph::asKnown(std::move(node), m_facts.value(node.id), m_book, known);
    const QList<TextMarks::Thing> things = TextMarks::known(met, place);
    auto matcher = std::make_shared<const TextMarks::Matcher>(things);
    const bool same = m_matcher && m_matcher->fingerprint() == matcher->fingerprint();
    m_things.clear();
    for (const TextMarks::Thing &thing : things)
        m_things.insert(thing.id, thing);
    m_matcher = std::move(matcher);
    if (!same || force)
        emit changed();
}

ReadingMarks::Key ReadingMarks::keyOf(int section, const QString &text) const {
    return {m_book, section, qHash(text), m_matcher ? m_matcher->fingerprint() : 0};
}

QString ReadingMarks::stampOf(const Key &key) const {
    return QString("%1:%2:%3:%4:%5").arg(key.book).arg(key.section).arg(key.text).arg(key.names).arg(m_look);
}

void ReadingMarks::keep(const Key &key, const Marks &marks) {
    if (!m_cache.contains(key))
        m_order.append(key);
    m_cache.insert(key, marks);
    while (m_order.size() > kept)
        m_cache.remove(m_order.takeFirst());
}

QList<TextMarks::Mark> ReadingMarks::marksIn(int section, const QString &text) {
    const Key key = keyOf(section, text);
    if (const auto found = m_cache.constFind(key); found != m_cache.cend()) {
        ++m_counts.hits;
        return *found;
    }
    ++m_counts.misses;
    const Marks marks = m_matcher ? m_matcher->match(text) : Marks();
    keep(key, marks);
    return marks;
}

void ReadingMarks::mark(QQuickTextDocument *chapter) {
    QTextDocument *document = chapter ? chapter->textDocument() : nullptr;
    if (!document)
        return;
    if (!m_matcher || m_matcher->isEmpty() || !m_reader->isOpen()) {
        clear(document);
        return;
    }
    const QString text = document->toPlainText();
    const Key key = keyOf(m_reader->section(), text);
    const QString stamp = stampOf(key);
    if (document->property(stampProperty).toString() == stamp) {
        // The same text, but maybe set anew (the reading format changed),
        // which took its formats along: then it is marked again.
        const auto found = m_cache.constFind(key);
        if (found == m_cache.cend() || stillMarked(document, *found))
            return;
        ++m_counts.hits;
        apply(document, *found, stamp);
        return;
    }
    if (const auto found = m_cache.constFind(key); found != m_cache.cend()) {
        ++m_counts.hits;
        apply(document, *found, stamp);
        return;
    }
    // Worked out beside the window: a long chapter and a large graph take
    // a few milliseconds, which a page turn shouldn't wait for.
    m_waiting[key].append(chapter);
    if (m_working.contains(key))
        return;
    ++m_counts.misses;
    auto watcher = std::make_unique<QFutureWatcher<Marks>>();
    connect(watcher.get(), &QFutureWatcher<Marks>::finished, this, [this, key] {
        const auto working = m_working.find(key);
        if (working == m_working.end())
            return;
        const Marks marks = working->second->result();
        // Not deleted from its own signal.
        working->second.release()->deleteLater();
        m_working.erase(working);
        keep(key, marks);
        const QString stamp = stampOf(key);
        bool laid = false;
        for (const QPointer<QQuickTextDocument> &waiting : m_waiting.take(key)) {
            QTextDocument *document = waiting ? waiting->textDocument() : nullptr;
            // Only on the text it was worked out for: the chapter may have
            // been turned, or set anew, in the meantime.
            if (document && m_book == key.book && m_matcher && m_matcher->fingerprint() == key.names
                && qHash(document->toPlainText()) == key.text) {
                apply(document, marks, stamp);
                laid = true;
            }
        }
        if (laid)
            emit marked();
    });
    watcher->setFuture(QtConcurrent::run([matcher = m_matcher, text] { return matcher->match(text); }));
    m_working.emplace(key, std::move(watcher));
}

void ReadingMarks::unmark(QQuickTextDocument *chapter) {
    if (chapter && chapter->textDocument())
        clear(chapter->textDocument());
}

void ReadingMarks::apply(QTextDocument *document, const Marks &marks, const QString &stamp) const {
    clear(document);
    // A read-only page has nothing to undo; marking it must not fill a stack.
    if (document->isUndoRedoEnabled())
        document->setUndoRedoEnabled(false);
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    const qsizetype end = document->characterCount() - 1;
    for (const TextMarks::Mark &mark : marks) {
        const auto thing = m_things.constFind(mark.thing);
        if (thing == m_things.cend() || mark.start + mark.length > end)
            continue;
        QTextCharFormat format;
        format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
        format.setUnderlineColor(colorOf(thing->kind));
        format.setBackground(tintOf(thing->kind));
        format.setProperty(thingProperty, mark.thing);
        cursor.setPosition(int(mark.start));
        cursor.setPosition(int(mark.start + mark.length), QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    cursor.endEditBlock();
    document->setProperty(stampProperty, stamp);
}

bool ReadingMarks::stillMarked(QTextDocument *document, const Marks &marks) const {
    const qsizetype end = document->characterCount() - 1;
    for (const TextMarks::Mark &mark : marks) {
        if (!m_things.contains(mark.thing) || mark.start + mark.length > end)
            continue;
        QTextCursor cursor(document);
        cursor.setPosition(int(mark.start + 1));
        return cursor.charFormat().hasProperty(thingProperty);
    }
    return true;
}

void ReadingMarks::clear(QTextDocument *document) const {
    if (document->property(stampProperty).toString().isEmpty())
        return;
    // Gathered first: changing a format splits and joins the fragments
    // being walked.
    QList<std::pair<int, QTextCharFormat>> marked;
    QList<int> lengths;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            QTextCharFormat format = fragment.charFormat();
            if (!format.hasProperty(thingProperty))
                continue;
            format.clearProperty(thingProperty);
            format.clearProperty(QTextFormat::TextUnderlineStyle);
            format.clearProperty(QTextFormat::TextUnderlineColor);
            format.clearProperty(QTextFormat::FontUnderline);
            format.clearBackground();
            marked.append({fragment.position(), format});
            lengths.append(fragment.length());
        }
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (qsizetype i = 0; i < marked.size(); ++i) {
        cursor.setPosition(marked.at(i).first);
        cursor.setPosition(marked.at(i).first + lengths.at(i), QTextCursor::KeepAnchor);
        cursor.setCharFormat(marked.at(i).second);
    }
    cursor.endEditBlock();
    document->setProperty(stampProperty, QString());
}

QVariantMap ReadingMarks::at(QQuickTextDocument *chapter, int position) const {
    QTextDocument *document = chapter ? chapter->textDocument() : nullptr;
    if (!document || position < 0)
        return {};
    // The position is between two characters; the one after it, or the one
    // before it at the end of a word.
    for (const int tried : {position, position - 1}) {
        if (tried < 0)
            continue;
        const QTextBlock block = document->findBlock(tried);
        QList<QTextFragment> fragments;
        for (auto it = block.begin(); !it.atEnd(); ++it)
            fragments << it.fragment();
        const auto idOf = [&fragments](qsizetype index) { return fragments.at(index).charFormat().property(thingProperty).toLongLong(); };
        const auto inside = std::ranges::find_if(fragments, [tried](const QTextFragment &fragment) {
            return tried >= fragment.position() && tried < fragment.position() + fragment.length();
        });
        if (inside == fragments.end())
            continue;
        qsizetype first = inside - fragments.begin(), last = first;
        const qint64 id = idOf(first);
        if (id == 0)
            continue;
        // A mark may be in pieces where the book's own formatting changes
        // inside a name ("<i>Ilse</i> Varga").
        while (first > 0 && idOf(first - 1) == id)
            --first;
        while (last + 1 < fragments.size() && idOf(last + 1) == id)
            ++last;
        QVariantMap found = thing(id);
        if (found.isEmpty())
            return {};
        found.insert("start", fragments.at(first).position());
        found.insert("end", fragments.at(last).position() + fragments.at(last).length());
        return found;
    }
    return {};
}

QString ReadingMarks::titleOf(int section) const {
    QString title;
    int best = -1;
    for (const QVariant &entry : m_reader->toc()) {
        const QVariantMap map = entry.toMap();
        const int at = map.value("section").toInt();
        if (at <= section && at > best) {
            best = at;
            title = map.value("title").toString();
        }
    }
    return title.isEmpty() ? tr("chapter %1").arg(section + 1) : title;
}

QVariantMap ReadingMarks::thing(qint64 id) const {
    const auto found = m_things.constFind(id);
    if (found == m_things.cend())
        return {};
    const Library *library = m_reader->isOpen() ? m_reader->library() : nullptr;
    const GraphPlace here = m_reader->place();
    const QString metAnchor = library ? Graph(*library).firstMet(id, m_book).value_or(GraphMoment()).anchor : QString();
    QVariantMap map{{"id", found->id},
                    {"kind", found->kind},
                    {"name", found->name},
                    {"summary", found->summary},
                    {"aliases", found->aliases},
                    {"from", found->from},
                    {"color", colorOf(found->kind)},
                    {"met", QVariantMap{{"section", found->met.section},
                                        {"fraction", found->met.fraction},
                                        {"anchor", metAnchor},
                                        {"title", titleOf(found->met.section)}}}};
    // Where the summary it has here was revealed, when that came later than
    // meeting it: the latest of the book's summaries up to the reader.
    // `latest` points into this list, so it is held here, not a temporary.
    const QList<GraphFact> facts = m_facts.value(id);
    const GraphFact *latest = nullptr;
    for (const GraphFact &fact : facts)
        if (fact.kind == "summary" && fact.revealed.place.bookId == m_book && fact.revealed.place.notAfter(here)
            && fact.value == found->summary && (!latest || latest->revealed.place.notAfter(fact.revealed.place)))
            latest = &fact;
    if (latest && !(latest->revealed.place.section == found->met.section && latest->revealed.place.fraction == found->met.fraction))
        map.insert("latest", QVariantMap{{"section", latest->revealed.place.section}, {"fraction", latest->revealed.place.fraction},
                                         {"anchor", latest->revealed.anchor}, {"title", titleOf(latest->revealed.place.section)}});
    // Its relations as the reader may know them, a few, the latest first.
    QVariantList relations;
    if (library) {
        const GraphView view = Graph(*library).ofBook(m_book, here);
        QHash<qint64, QString> names;
        for (const GraphNode &node : view.nodes)
            names.insert(node.id, node.name);
        for (const GraphEdge &edge : view.edges | std::views::reverse) {
            if (relations.size() >= 4)
                break;
            const bool outgoing = edge.source == id;
            if ((!outgoing && edge.target != id) || edge.across())
                continue;
            const qint64 other = outgoing ? edge.target : edge.source;
            relations << QVariantMap{{"relation", edge.relation()}, {"other", names.value(other)}, {"otherId", other}, {"outgoing", outgoing}};
        }
    }
    map.insert("relations", relations);
    return map;
}

QVariantMap ReadingMarks::named(const QString &words) const {
    const qint64 id = m_matcher ? m_matcher->named(words) : 0;
    return id ? thing(id) : QVariantMap();
}

QString ReadingMarks::suggestionKey(qint64 id, const QString &words) const {
    return QString("%1:%2:%3").arg(m_book).arg(id > 0 ? QString::number(id) : words.simplified().toCaseFolded()).arg(m_reader->section());
}

void ReadingMarks::keepSuggestions(const QString &key, const QStringList &questions) {
    m_suggested.insert(key, questions);
    emit suggestionsChanged();
}

QVariantList ReadingMarks::suggestions(qint64 id, const QString &words) {
    const QString key = suggestionKey(id, words);
    QVariantList list;
    const QString made = key + ":" + QString::number(m_matcher ? m_matcher->fingerprint() : 0);
    if (const auto cached = m_madeSuggestions.constFind(made); cached != m_madeSuggestions.cend()) {
        list = *cached;
    } else if (const auto found = m_things.constFind(id); found != m_things.cend() && m_reader->library()) {
        // Made from the graph, with no AI: its threads, places and relations.
        const GraphView view = Graph(*m_reader->library()).ofBook(m_book, m_reader->place());
        const auto node = std::ranges::find(view.nodes, id, &GraphNode::id);
        if (node != view.nodes.end())
            for (const TextMarks::Suggestion &one : TextMarks::suggestions(*node, found->met, view, m_reader->place())) {
                using Shape = TextMarks::Suggestion::Shape;
                const QString text = one.shape == Shape::Thread ? tr("What does %1 have to do with %2?").arg(one.first, one.second)
                                     : one.shape == Shape::Place ? tr("Why is %1 at %2?").arg(one.first, one.second)
                                     : one.shape == Shape::Relation ? tr("Why is %1 %2 %3?").arg(one.first, one.relation, one.second)
                                     : one.shape == Shape::Between ? tr("How are %1 and %2 connected?").arg(one.first, one.second)
                                     : one.shape == Shape::Before ? tr("What happened to %1 before?").arg(one.first)
                                                                  : tr("What do we know of %1 so far?").arg(one.first);
                list << QVariantMap{{"text", text}, {"ai", false}};
            }
        m_madeSuggestions.insert(made, list);
    }
    for (const QString &question : m_suggested.value(key))
        list << QVariantMap{{"text", question}, {"ai", true}};
    return list;
}

QString ReadingMarks::linked(const QString &markdown) const {
    if (!m_matcher || m_matcher->isEmpty() || markdown.trimmed().isEmpty())
        return {};
    QTextDocument document;
    document.setMarkdown(markdown);
    const Marks marks = m_matcher->match(document.toPlainText());
    if (marks.isEmpty())
        return {};
    QTextCursor cursor(&document);
    for (const TextMarks::Mark &mark : marks) {
        const auto thing = m_things.constFind(mark.thing);
        if (thing == m_things.cend())
            continue;
        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorHref(QString("omabook-thing:%1").arg(mark.thing));
        format.setForeground(m_foreground);
        format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
        format.setUnderlineColor(colorOf(thing->kind));
        format.setBackground(tintOf(thing->kind));
        cursor.setPosition(int(mark.start));
        cursor.setPosition(int(mark.start + mark.length), QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    // The pane's own type, not the font the document was made with.
    // (An ordinary literal: the catalog's reader takes a raw one for the end of a string.)
    static const QRegularExpression bodyStyle("<body style=\"[^\"]*\">");
    return document.toHtml().replace(bodyStyle, "<body>");
}

QString ReadingMarks::paragraph(QQuickTextDocument *chapter, int position) const {
    QTextDocument *document = chapter ? chapter->textDocument() : nullptr;
    return document ? document->findBlock(position).text() : QString();
}

QVariantMap ReadingMarks::wordAt(QQuickTextDocument *chapter, int position) const {
    QTextDocument *document = chapter ? chapter->textDocument() : nullptr;
    if (!document)
        return {};
    QTextCursor cursor(document);
    cursor.setPosition(qBound(0, position, document->characterCount() - 1));
    cursor.select(QTextCursor::WordUnderCursor);
    return {{"start", cursor.selectionStart()}, {"end", cursor.selectionEnd()}};
}

int ReadingMarks::wordFrom(QQuickTextDocument *chapter, int position, int steps) const {
    QTextDocument *document = chapter ? chapter->textDocument() : nullptr;
    if (!document)
        return position;
    QTextCursor cursor(document);
    cursor.setPosition(qBound(0, position, document->characterCount() - 1));
    cursor.movePosition(steps < 0 ? QTextCursor::PreviousWord : QTextCursor::NextWord, QTextCursor::MoveAnchor, std::abs(steps));
    return cursor.position();
}

QString ReadingMarks::notedUpTo() {
    Library *library = m_reader->isOpen() && enabled() ? m_reader->library() : nullptr;
    if (!library)
        return {};
    const auto built = GraphBuild::builtTo(*library, m_book);
    const GraphPlace here = m_reader->place();
    // Asked once per place the graph got to and chapter the reader is in:
    // it reads the pages between the two.
    const QString asked = built ? QString("%1:%2:%3:%4").arg(m_book).arg(built->section).arg(built->fraction).arg(here.section)
                                : QString("%1:none:%2").arg(m_book).arg(here.section);
    if (asked == m_notedFor)
        return m_noted;
    m_notedFor = asked;
    // The builder sends only what was read since, when a chapter is
    // finished; the page doesn't ask it to. It only says how far it got.
    const qsizetype behind = GraphBuild::size(m_reader->passagesSince(built));
    if (behind < GraphBuild::worthAsking)
        m_noted.clear();
    else if (!built)
        m_noted = tr("Nothing of this book is noted in the graph yet");
    else
        m_noted = tr("Noted up to %1").arg(titleOf(built->section));
    return m_noted;
}

void ReadingMarks::goToMoment(int section, double fraction, const QString &words) {
    const QString wanted = words.simplified();
    double at = fraction;
    // The words in that section, the nearest to the place kept with them:
    // the place itself is close, the words are exact.
    if (wanted.size() >= 2) {
        double nearest = 2;
        for (const QVariant &result : m_reader->find(wanted)) {
            const QVariantMap found = result.toMap();
            const double there = found.value("fraction").toDouble();
            if (found.value("section").toInt() == section && std::abs(there - fraction) < nearest) {
                nearest = std::abs(there - fraction);
                at = there;
            }
        }
    }
    m_flash = {{"section", section}, {"fraction", at}, {"words", wanted}};
    m_reader->goToPlace(section, at);
    emit flashChanged();
    m_flashOver->start();
}

QVariantMap ReadingMarks::findIn(QQuickTextDocument *chapter, const QString &words, double fraction) const {
    const QTextDocument *document = chapter ? chapter->textDocument() : nullptr;
    const auto found = document ? TextMarks::nearest(*document, words, fraction) : std::nullopt;
    return found ? QVariantMap{{"start", found->first}, {"end", found->second}} : QVariantMap();
}
