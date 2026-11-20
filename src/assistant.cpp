#include "assistant.h"

#include "graphcontext.h"
#include "prompts.h"
#include "reader.h"

Assistant::Assistant(Reader *reader, Factory factory, QObject *parent)
    : QObject(parent), m_reader(reader), m_factory(std::move(factory)),
      m_graph([this](QString *why) { return m_factory(why); }, [this] { return m_reader->library(); }) {
    // Another book is another conversation.
    // (The same book read anew, because it grew, is the same conversation.)
    connect(m_reader, &Reader::bookChanged, this, [this] {
        if (!m_reader->growing())
            clear();
    });
    // The knowledge graph grows as chapters are finished: a moment after
    // the reader moves on to the next one, so leafing through costs nothing.
    // Only the step from a chapter to the one after it counts; a leap
    // through the contents is not reading.
    m_graphSoon.setSingleShot(true);
    m_graphSoon.setInterval(3000);
    connect(&m_graphSoon, &QTimer::timeout, this, [this] { grow(false); });
    connect(m_reader, &Reader::bookChanged, this, [this] {
        if (m_reader->growing())
            return;
        m_graphSoon.stop();
        m_graph.cancel();
        m_lastSection = m_reader->isOpen() ? m_reader->section() : -1;
    });
    connect(m_reader, &Reader::sectionChanged, this, [this] {
        const int section = m_reader->section();
        if (m_reader->isOpen() && m_lastSection >= 0 && section == m_lastSection + 1 && graphMode() == "auto")
            m_graphSoon.start();
        m_lastSection = section;
    });
    connect(&m_graph, &GraphBuilder::busyChanged, this, &Assistant::graphChanged);
    connect(&m_graph, &GraphBuilder::grown, this, &Assistant::graphChanged);
    refresh();
}

void Assistant::refresh() {
    cancel();
    m_problem.clear();
    m_provider = m_factory(&m_problem);
    if (m_provider) {
        // Every answer here is for the reader to read: without the markup some models wrap it in.
        connect(m_provider.get(), &AiProvider::delta, this, [this](const QString &text) {
            const QString shown = m_markup.take(text);
            if (shown.isEmpty())
                return;
            m_streaming += shown;
            emit streamingChanged();
        });
        connect(m_provider.get(), &AiProvider::finished, this, [this] {
            finishAnswer();
            setBusy(false);
        });
        // A model that was refused, and which answered instead.
        connect(m_provider.get(), &AiProvider::noted, this, [this](const QString &note) {
            m_note = note;
            emit messagesChanged();
        });
        connect(m_provider.get(), &AiProvider::failed, this, [this](const QString &error) {
            m_failed = true;
            m_note = error;
            finishAnswer();
            setBusy(false);
        });
    }
    emit availabilityChanged();
}

QVariantList Assistant::messages() const {
    QVariantList list;
    for (const AiMessage &message : m_messages)
        list << QVariantMap{{"role", message.role}, {"text", message.text}};
    return list;
}

void Assistant::ask(const QString &question, const QString &scope, const QString &selection) {
    if (!m_provider || m_busy || question.trimmed().isEmpty() || !m_reader->isOpen())
        return;
    const Prompts::Excerpt excerpt = Prompts::fit(scope == "sofar" ? m_reader->textSoFar() : m_reader->sectionText());
    // Nothing is cut without saying so.
    m_note = excerpt.trimmed ? tr("The book so far is longer than fits in one question, so only its most recent part was sent.")
                             : QString();

    // Earlier turns go as they were asked; only the newest carries the text,
    // since that is where the reader is now.
    AiRequest request;
    request.system = Prompts::companion(m_reader->meta(), m_reader->seriesNotes("recap"));
    request.messages = m_messages;
    // What the graph holds on the things the question names, as far as the
    // reader has got: a few lines that keep the answers consistent.
    const Library *library = m_reader->library();
    const QString known = library && graphMode() != "off"
                              ? GraphContext::forQuestion(Graph::ofReader(*library, m_reader->bookId()), m_reader->bookId(),
                                                          m_reader->place(), question + "\n" + selection)
                              : QString();
    request.messages.append({"user", Prompts::question(excerpt.text, selection, question, known)});
    m_asked = question.trimmed();
    m_askedAt = m_reader->place();

    m_messages.append({"user", question.trimmed()});
    emit messagesChanged();
    setBusy(true);
    m_provider->ask(request);
}

void Assistant::identify(const QString &selection, const QString &paragraph) {
    if (!m_provider || m_busy || selection.trimmed().isEmpty() || !m_reader->isOpen())
        return;
    m_note.clear();
    AiRequest request;
    request.system = Prompts::companion(m_reader->meta(), m_reader->seriesNotes("recap"));
    request.messages.append({"user", Prompts::identify(selection, paragraph)});
    m_messages.append({"user", tr("Who or what is “%1”?").arg(selection.simplified().left(80))});
    emit messagesChanged();
    setBusy(true);
    m_provider->ask(request);
}

void Assistant::suggestMore(const QString &key, qint64 thing, const QString &selection, const QString &paragraph) {
    ReadingMarks *marks = m_reader->marks();
    if (m_suggesting || !m_reader->isOpen() || marks->suggested(key))
        return;
    QString why;
    std::unique_ptr<AiProvider> provider = m_factory(&why);
    if (!provider)
        return;
    const QVariantMap known = marks->thing(thing);
    // Only the selection, its paragraph and the one line the graph holds of it.
    const QString line = known.isEmpty() ? QString() : known.value("name").toString() + ": " + known.value("summary").toString();
    m_suggesting = std::make_unique<AiCall>(std::move(provider));
    // A failed call keeps nothing, so asking again tries again.
    const auto done = [this, key](const std::optional<QStringList> &questions) {
        if (questions)
            m_reader->marks()->keepSuggestions(key, *questions);
        m_suggesting.release()->deleteLater();
        emit suggestingChanged();
    };
    connect(m_suggesting.get(), &AiCall::done, this, [done](const QString &answer) { done(Prompts::questionsIn(answer)); });
    connect(m_suggesting.get(), &AiCall::failed, this, [done] { done(std::nullopt); });
    emit suggestingChanged();
    m_suggesting->ask(Prompts::moreQuestions(m_reader->meta(), selection, paragraph, line));
}

void Assistant::catchUp() { askFromNotes("recap", tr("Catch me up.")); }

void Assistant::whoIsWho() { askFromNotes("cast", tr("Who is who?")); }

void Assistant::askFromNotes(const QString &kind, const QString &asked) {
    if (!m_provider || m_busy || !m_reader->isOpen())
        return;
    const Recap notes = m_reader->notes(kind);
    QString since = notes.text.isEmpty() ? m_reader->textSoFar() : m_reader->textSince(notes.place.section, notes.place.fraction);
    // The first time, the knowledge graph stands in for the notes there
    // aren't yet: what it holds of the book goes, with only the pages read
    // since it was built, in place of the whole book so far.
    QString from = notes.text;
    const Library *library = m_reader->library();
    const auto built = library && notes.text.isEmpty() && graphMode() != "off"
                           ? GraphBuild::builtTo(*library, m_reader->bookId()) : std::nullopt;
    if (built && built->notAfter(m_reader->place())) {
        const QString digest = GraphContext::digest(Graph::ofReader(*library, m_reader->bookId()), m_reader->bookId(), *built,
                                                    kind == "cast" ? QStringList{"character", "place", "group"} : Prompts::graphKinds());
        if (!digest.isEmpty()) {
            from = Prompts::graphNotes(digest);
            since = m_reader->textSince(built->section, built->fraction);
        }
    }
    m_messages.append({"user", asked});
    // Barely a page read since the last time: the notes still stand, and
    // showing them again costs nothing.
    if (!notes.text.isEmpty() && since.size() < 1500) {
        m_messages.append({"assistant", notes.text});
        m_note = tr("From the last time you asked; you have read little since, so nothing was sent.");
        emit messagesChanged();
        return;
    }
    emit messagesChanged();
    // Earlier books of the series that nothing is known of are looked up
    // first, once: what a catalog says they are about is kept with them.
    const QList<Book> unknown = notes.text.isEmpty() ? m_reader->unknownEarlierBooks() : QList<Book>();
    if (unknown.isEmpty()) {
        sendFromNotes(kind, from, since, notes.text.isEmpty());
        return;
    }
    setBusy(true);
    m_search = std::make_unique<BlurbSearch>(unknown, [this](QObject *parent) { return m_reader->newSources(parent); });
    connect(m_search.get(), &BlurbSearch::finished, this, [this, kind, from, since](const QHash<qint64, QString> &blurbs) {
        for (auto blurb = blurbs.begin(); blurb != blurbs.end(); ++blurb)
            m_reader->setBlurb(blurb.key(), blurb.value());
        m_search.release()->deleteLater();
        setBusy(false);
        sendFromNotes(kind, from, since, true);
    });
}

void Assistant::sendFromNotes(const QString &kind, const QString &notes, const QString &since, bool first) {
    const Prompts::Excerpt excerpt = Prompts::fit(since);
    m_note = excerpt.trimmed ? tr("The book so far is longer than fits in one question, so only its most recent part was sent.")
                             : QString();
    // The earlier books of a series go along once: after that these notes
    // carry what matters of them.
    const QList<SeriesNotes> earlier = first ? m_reader->seriesNotes(kind) : QList<SeriesNotes>();
    if (!earlier.isEmpty()) {
        QStringList titles;
        for (const SeriesNotes &book : earlier)
            titles << book.title;
        m_note = (m_note.isEmpty() ? QString() : m_note + " ")
                 + tr("Builds on what is known of the earlier books of the series: %1.").arg(titles.join(", "));
    }
    AiRequest request;
    request.system = Prompts::companion(m_reader->meta());
    request.messages.append({"user", kind == "cast" ? Prompts::cast(notes, excerpt.text, earlier)
                                                    : Prompts::catchUp(notes, excerpt.text, earlier)});
    emit messagesChanged();
    m_noting = kind;
    setBusy(true);
    m_provider->ask(request);
}

// What has been written joins the conversation, whether the answer ended,
// failed partway, or was stopped.
void Assistant::finishAnswer() {
    m_streaming += m_markup.finish();
    // A whole catch-up becomes the notes the next one starts from, and is
    // when the graph is brought up to where the reader is too.
    const bool whole = !m_failed && !m_streaming.trimmed().isEmpty();
    if (!m_noting.isEmpty() && whole) {
        m_reader->setNotes(m_noting, m_streaming.trimmed());
        if (graphMode() == "auto")
            grow(false);
    }
    m_noting.clear();
    // What was asked, with the gist of its answer, joins the graph.
    Library *library = m_reader->library();
    if (!m_asked.isEmpty() && whole && library && graphMode() != "off" && m_reader->bookId() == m_askedAt.bookId)
        GraphContext::noteQuestion(Graph::ofReader(*library, m_askedAt.bookId), m_askedAt.bookId, m_askedAt, m_asked,
                                   m_streaming.trimmed());
    m_asked.clear();
    m_failed = false;
    if (!m_streaming.trimmed().isEmpty())
        m_messages.append({"assistant", m_streaming.trimmed()});
    m_streaming.clear();
    emit streamingChanged();
    emit messagesChanged();
}

void Assistant::cancel() {
    // A look-up under way is dropped; what it would have found is found next time.
    if (m_search) {
        m_search.reset();
        setBusy(false);
        return;
    }
    if (m_provider && m_busy) {
        m_provider->cancel();
        // Half an answer is shown, but is nobody's notes.
        m_noting.clear();
        m_asked.clear();
        finishAnswer();
    }
    setBusy(false);
}

void Assistant::growGraph() { grow(true); }

// `asked` is the reader's own request: everything read goes, however
// little. Left to itself it waits for a few pages and sends a few requests
// at most, so one long evening's reading is not one long bill.
void Assistant::grow(bool asked) {
    Library *library = m_reader->library();
    if (!m_provider || !library || !m_reader->isOpen() || m_graph.busy() || graphMode() == "off")
        return;
    const auto built = GraphBuild::builtTo(*library, m_reader->bookId());
    if (built && m_reader->place().notAfter(*built))
        return;
    const QList<GraphBuild::Passage> passages = m_reader->passagesSince(built);
    if (GraphBuild::size(passages) >= (asked ? 1 : GraphBuild::worthAsking))
        m_graph.grow(m_reader->bookId(), passages, asked ? 1000 : 3, asked || graphMode() == "auto");
}

void Assistant::clear() {
    cancel();
    m_messages.clear();
    m_note.clear();
    emit messagesChanged();
}

void Assistant::setBusy(bool busy) {
    if (busy == m_busy)
        return;
    m_busy = busy;
    emit busyChanged();
}
