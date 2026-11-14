#include "librarian.h"

#include "graphcontext.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

#include "backend.h"
#include "goodreads.h"
#include "interfacechange.h"
#include "metadatasource.h"
#include "settings.h"

Librarian::Librarian(Backend *backend, Settings *settings, Customization *custom, Colors colors, QObject *parent)
    : QObject(parent), m_backend(backend), m_settings(settings), m_custom(custom), m_colors(std::move(colors)),
      m_desk(settings, &m_network, [backend] { return backend->library() ? backend->library()->root() : QString(); }) {
    connect(settings, &Settings::changed, this, &Librarian::aiChanged);
    // What the reading sites bring shows on the shelf, covers and all.
    connect(&m_desk, &SiteDesk::libraryChanged, backend, &Backend::reload);
    connect(&m_desk, &SiteDesk::coversWanted, this, [this](const QList<QPair<qint64, QUrl>> &covers) { m_covers.fetch(covers); });
    m_sites = [this](QObject *parent) { return ReadingSites::all(*m_settings, &m_network, parent); };
    connect(backend, &Backend::readingFilePicked, this, &Librarian::readingFilePicked);
    // Covers arrive one by one; the shelf is drawn again now and then, and
    // once more when the last one is in.
    connect(&m_covers, &CoverFetcher::cover, this, [this](qint64 id, const QImage &picture) {
        if (m_backend->library() && m_backend->library()->setCover(id, picture) && ++m_coversArrived % 12 == 0)
            m_backend->reload();
    });
    connect(&m_covers, &CoverFetcher::done, this, [this] { m_backend->reload(); });
}

Librarian::~Librarian() { stop(); }

bool Librarian::hasAi() const { return m_backend->newAi(nullptr) != nullptr; }

QString Librarian::aiProblem() const {
    QString why;
    m_backend->newAi(&why);
    return why;
}

bool Librarian::canUndo() const { return InterfaceChanges::canUndo(*m_custom); }

QVariantList Librarian::proposals() const {
    QVariantList list;
    Library *library = m_backend->library();
    for (const Entry &entry : m_entries) {
        const Proposal &proposal = entry.proposal;
        const auto book = library ? library->book(proposal.bookId) : std::nullopt;
        const auto other = library ? library->book(proposal.otherId) : std::nullopt;
        QVariantList changes;
        QStringList adds;
        if (book && proposal.kind != "duplicate") {
            // What the book would be, against what it is.
            MetaCandidate would;
            would.meta = Curation::after(book->meta, proposal);
            would.coverUrl = proposal.coverUrl;
            const MetaDiff diff = whatItBrings(book->meta, book->hasCover, would);
            adds = diff.adds;
            for (const MetaDiff::Change &change : diff.changes)
                changes << QVariantMap{{"field", change.field}, {"from", change.from}, {"to", change.to}};
        }
        list << QVariantMap{
            {"kind", proposal.kind},
            {"title", book ? book->meta.title : QString()},
            {"authors", book ? book->meta.authorText() : QString()},
            {"otherTitle", other ? other->meta.title : QString()},
            {"otherFormats", other ? other->formats.join(", ") : QString()},
            {"formats", book ? book->formats.join(", ") : QString()},
            {"adds", adds},
            {"changes", changes},
            {"why", proposal.why},
            {"chosen", entry.chosen},
        };
    }
    return list;
}

int Librarian::chosen() const {
    return int(std::ranges::count(m_entries, true, &Entry::chosen));
}

void Librarian::setBusy(bool busy, const QString &status) {
    m_busy = busy;
    m_status = status;
    emit busyChanged();
}

void Librarian::add(const QList<Proposal> &proposals) {
    for (const Proposal &proposal : proposals)
        m_entries.append({proposal, true});
    emit proposalsChanged();
}

int Librarian::incomplete() const {
    const QList<Book> books = m_backend->library() ? m_backend->library()->books() : QList<Book>();
    return int(std::ranges::count_if(books, [](const Book &book) { return book.meta.description.isEmpty() || !book.hasCover; }));
}

void Librarian::tidy(bool lookUps) {
    stop();
    Library *library = m_backend->library();
    if (!library)
        return;
    m_entries.clear();
    m_lookUps = lookUps;
    const QList<Book> books = library->books();
    // What needs no AI is there at once.
    add(Curation::duplicates(books));
    add(Curation::authorSpellings(books));
    if (!hasAi()) {
        emit proposalsChanged();
        return;
    }
    askSeries();
}

void Librarian::askSeries() {
    const QList<Book> books = m_backend->library()->books();
    if (books.isEmpty()) {
        finishTidy();
        return;
    }
    setBusy(true, tr("Looking for series…"));
    m_tidyCall = std::make_unique<AiCall>(m_backend->newAi(nullptr));
    connect(m_tidyCall.get(), &AiCall::done, this, [this, books](const QString &answer) {
        // A book the tidy would fold away needs no series of its own.
        QList<Proposal> series = Curation::seriesFrom(answer, books);
        for (const Entry &entry : std::as_const(m_entries))
            series.removeIf([&](const Proposal &proposal) {
                return entry.proposal.kind == "duplicate" && proposal.bookId == entry.proposal.otherId;
            });
        add(series);
        finishTidy();
    });
    connect(m_tidyCall.get(), &AiCall::failed, this, [this](const QString &error) {
        m_backend->say(error);
        finishTidy();
    });
    m_tidyCall->ask(Curation::seriesRequest(books));
}

// After the series, the look-ups, if they were asked for: one book at a
// time, each judged by the AI against the book's own opening pages.
void Librarian::finishTidy() {
    m_toLookUp.clear();
    if (m_lookUps && hasAi()) {
        QSet<qint64> folded;
        for (const Entry &entry : std::as_const(m_entries))
            if (entry.proposal.kind == "duplicate")
                folded << entry.proposal.otherId;
        for (const Book &book : m_backend->library()->books())
            if ((book.meta.description.isEmpty() || !book.hasCover) && !folded.contains(book.id))
                m_toLookUp << book.id;
    }
    m_lookUps = false;
    m_lookUpTotal = m_toLookUp.size();
    lookUpNext();
}

void Librarian::lookUpNext() {
    m_lookUp.reset();
    Library *library = m_backend->library();
    if (m_toLookUp.isEmpty() || !library) {
        setBusy(false);
        return;
    }
    const qint64 id = m_toLookUp.takeFirst();
    const auto book = library->book(id);
    if (!book) {
        lookUpNext();
        return;
    }
    setBusy(true, tr("Looking up %1 of %2: %3").arg(m_lookUpTotal - m_toLookUp.size()).arg(m_lookUpTotal).arg(book->meta.title));

    // Everything of one look-up hangs off one object, deleted when it is done.
    m_lookUp = std::make_unique<QObject>();
    const QList<MetadataSource *> sources = m_backend->newSources(m_lookUp.get());
    auto found = std::make_shared<QList<MetaCandidate>>();
    auto waiting = std::make_shared<int>(sources.size());
    auto judge = [this, id, found] {
        const auto book = m_backend->library() ? m_backend->library()->book(id) : std::nullopt;
        m_judge = m_backend->newAi(nullptr);
        if (!book || !m_judge) {
            lookUpNext();
            return;
        }
        auto *source = new AiMetadataSource(m_judge.get(), QFileInfo(m_backend->library()->formatPath(id)).fileName(),
                                            m_backend->openingPagesOf(id), m_lookUp.get());
        source->setFound(*found);
        connect(source, &MetadataSource::found, this, [this, id](const QList<MetaCandidate> &judged) {
            const auto book = m_backend->library() ? m_backend->library()->book(id) : std::nullopt;
            if (book && !judged.isEmpty()) {
                const MetaDiff diff = whatItBrings(book->meta, book->hasCover, judged.first());
                // Worth proposing only when it would change something.
                if (!diff.adds.isEmpty() || !diff.changes.isEmpty())
                    add({{"details", id, 0, judged.first().meta, judged.first().coverUrl, judged.first().why}});
            }
            lookUpNext();
        });
        connect(source, &MetadataSource::failed, this, [this] { lookUpNext(); });
        source->search(book->meta);
    };
    if (sources.isEmpty()) {
        judge();
        return;
    }
    for (MetadataSource *source : sources) {
        connect(source, &MetadataSource::found, this, [found, waiting, judge](const QList<MetaCandidate> &candidates) {
            *found << candidates;
            if (--*waiting == 0) judge();
        });
        connect(source, &MetadataSource::failed, this, [waiting, judge] { if (--*waiting == 0) judge(); });
        source->search(book->meta);
    }
}

void Librarian::setChosen(int index, bool chosen) {
    if (index < 0 || index >= m_entries.size())
        return;
    m_entries[index].chosen = chosen;
    emit proposalsChanged();
}

void Librarian::applyChosen() {
    Library *library = m_backend->library();
    if (!library || m_busy)
        return;
    int applied = 0;
    QStringList problems;
    // Details and spellings first, then the merges, which take books away.
    for (const bool merges : {false, true}) {
        for (const Entry &entry : std::as_const(m_entries)) {
            const Proposal &proposal = entry.proposal;
            if (!entry.chosen || (proposal.kind == "duplicate") != merges)
                continue;
            QString error;
            if (Curation::apply(*library, proposal, &error)) {
                ++applied;
                if (proposal.coverUrl.isValid())
                    m_backend->fetchCoverFor(proposal.bookId, proposal.coverUrl);
            } else {
                problems << error;
            }
        }
    }
    m_entries.clear();
    emit proposalsChanged();
    m_backend->reload();
    m_backend->say((QStringList{tr("%Ln changes made", "", applied)} + problems.mid(0, 3)).join("\n"));
}

void Librarian::stop() {
    if (m_tidyCall)
        m_tidyCall->cancel();
    m_tidyCall.reset();
    m_toLookUp.clear();
    m_lookUp.reset();
    if (m_judge)
        m_judge->cancel();
    m_judge.reset();
    if (m_busy)
        setBusy(false);
}

void Librarian::ask(const QString &question) {
    Library *library = m_backend->library();
    QString why;
    std::unique_ptr<AiProvider> provider = m_backend->newAi(&why);
    if (!library || question.trimmed().isEmpty() || m_answering)
        return;
    m_answer = provider ? QString() : why;
    if (!provider) {
        emit answerChanged();
        return;
    }
    QList<Curation::BookNotes> books;
    for (const Book &book : library->books())
        books.append({book, library->notes(book.id, "recap").text});
    // The knowledge graph's picture of what was read goes along, and the
    // question joins the graph once it is answered; neither with it off.
    const bool graph = m_settings->graph() != "off";
    m_question = graph ? question.trimmed() : QString();
    startAnswer(std::move(provider),
                Curation::libraryQuestion(books, question, graph ? GraphContext::picture(*library, question) : QString()));
}

void Librarian::startAnswer(std::unique_ptr<AiProvider> provider, const AiRequest &request) {
    m_checking.reset();
    m_suggestions.clear();
    m_answering = true;
    emit answerChanged();
    m_askCall = std::make_unique<AiCall>(std::move(provider));
    connect(m_askCall.get(), &AiCall::delta, this, [this](const QString &text) {
        m_answer += text;
        emit answerChanged();
    });
    connect(m_askCall.get(), &AiCall::done, this, [this](const QString &answer) {
        if (Library *library = m_backend->library(); library && !m_question.isEmpty())
            GraphContext::noteQuestion(*library, 0, std::nullopt, m_question, answer);
        m_question.clear();
        m_answering = false;
        emit answerChanged();
    });
    connect(m_askCall.get(), &AiCall::failed, this, [this](const QString &error) {
        m_answer = error;
        m_answering = false;
        emit answerChanged();
    });
    m_askCall->ask(request);
}

void Librarian::suggest() {
    Library *library = m_backend->library();
    QString why;
    std::unique_ptr<AiProvider> provider = m_backend->newAi(&why);
    if (!library || m_answering)
        return;
    m_answer = provider ? QString() : why;
    if (!provider) {
        emit answerChanged();
        return;
    }
    QList<Curation::BookNotes> books;
    for (const Book &book : library->books())
        books.append({book, library->notes(book.id, "recap").text});
    // The answer is JSON to be checked, not prose to show as it is written.
    m_checking.reset();
    m_suggestions.clear();
    m_answering = true;
    emit answerChanged();
    m_askCall = std::make_unique<AiCall>(std::move(provider));
    connect(m_askCall.get(), &AiCall::done, this, [this](const QString &answer) {
        const Curation::Suggestions suggested = Curation::suggestionsFrom(answer);
        // A model that answered in its own words is shown in its own words.
        m_answer = suggested.books.isEmpty() ? answer.trimmed() : suggested.first;
        m_answering = false;
        emit answerChanged();
        if (!suggested.books.isEmpty())
            checkSuggestions(suggested);
    });
    connect(m_askCall.get(), &AiCall::failed, this, [this](const QString &error) {
        m_answer = error;
        m_answering = false;
        emit answerChanged();
    });
    m_askCall->ask(Curation::suggestionRequest(books, m_settings->graph() != "off" ? GraphContext::picture(*library) : QString()));
}

void Librarian::checkSuggestions(const Curation::Suggestions &suggested) {
    Library *library = m_backend->library();
    if (!library)
        return;
    // The sites and the check hang off one object, gone with the next question.
    m_checking = std::make_unique<QObject>();
    m_check = new SuggestionCheck(m_sites(m_checking.get()), m_checking.get());
    connect(m_check, &SuggestionCheck::changed, this, [this] {
        m_suggestions = m_check->results();
        emit answerChanged();
    });
    m_check->check(suggested.books, library->books());
}

QVariantList Librarian::suggestions() const {
    QVariantList list;
    for (const ReadingSites::Checked &checked : m_suggestions) {
        const bool found = checked.state == "found";
        QString note;
        if (checked.bookId > 0)
            note = checked.shelf == "to-read" ? tr("On your to-read list")
                   : checked.shelf == "read" ? tr("You have read it")
                   : checked.shelf == "reading" ? tr("You are reading it")
                   : tr("Already in your library");
        else if (checked.state == "missing")
            note = tr("No reading site knows this book");
        else if (checked.state == "checking")
            note = tr("Looking it up…");
        list << QVariantMap{
            {"title", found ? checked.book.title : checked.suggestion.title},
            {"author", found && !checked.book.authors.isEmpty() ? checked.book.authors.join(" & ") : checked.suggestion.author},
            {"why", checked.suggestion.why},
            {"state", checked.state},
            {"rating", found && checked.book.averageRating > 0
                           ? tr("%1 on %2").arg(QLocale().toString(checked.book.averageRating, 'f', 2), checked.book.site)
                           : QString()},
            {"note", note},
            // A book no site knows may not exist; one already on a shelf needs no adding.
            {"canWant", checked.state != "missing" && checked.state != "checking" && (checked.bookId == 0 || checked.shelf.isEmpty())},
        };
    }
    return list;
}

void Librarian::wantSuggestion(int index) {
    Library *library = m_backend->library();
    if (!library || index < 0 || index >= m_suggestions.size())
        return;
    ReadingSites::Checked &checked = m_suggestions[index];
    const bool isNew = checked.bookId == 0;
    QString error;
    const qint64 id = ReadingSites::addToRead(*library, checked, &error);
    if (id == 0) {
        m_backend->say(error);
        return;
    }
    checked.bookId = id;
    checked.shelf = "to-read";
    checked.held = "none";
    if (isNew && checked.book.coverUrl.isValid())
        m_covers.fetch({{id, checked.book.coverUrl}});
    m_backend->reload();
    emit answerChanged();
}

void Librarian::importSiteFile() { m_backend->pickReadingFile(false); }

void Librarian::exportSiteFile() {
    m_backend->pickReadingFile(true, QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/omabook_for_goodreads.csv");
}

void Librarian::readingFilePicked(bool save, const QString &file) {
    Library *library = m_backend->library();
    if (!library || file.isEmpty())
        return;
    if (!save) {
        m_desk.importFile(file);
        return;
    }
    const QList<Book> books = library->books();
    const QByteArray made = Goodreads::writeCsv(books);
    QSaveFile target(file);
    target.setDirectWriteFallback(false);
    if (!target.open(QIODevice::WriteOnly) || target.write(made) != made.size() || !target.commit()) {
        m_desk.note("goodreads", tr("The file can't be written to %1.").arg(file));
        return;
    }
    const int written = int(std::ranges::count_if(books, [](const Book &book) { return !book.status().isEmpty(); }));
    m_desk.note("goodreads", tr("%Ln books written to %1. Upload the file at goodreads.com/review/import to bring Goodreads in step.", "",
                                written).arg(file));
}

void Librarian::change(const QString &wish) {
    QString why;
    std::unique_ptr<AiProvider> provider = m_backend->newAi(&why);
    if (wish.trimmed().isEmpty() || m_changing)
        return;
    m_changeNote = provider ? QString() : why;
    m_changing = provider != nullptr;
    emit changeChanged();
    if (!provider)
        return;
    m_changeCall = std::make_unique<AiCall>(std::move(provider));
    connect(m_changeCall.get(), &AiCall::done, this, [this](const QString &answer) {
        const Prompts::InterfaceChange change = Prompts::parseChange(answer);
        const QString problem = InterfaceChanges::apply(*m_custom, *m_settings, change);
        m_changeNote = !problem.isEmpty() ? tr("Nothing was changed: %1").arg(problem)
                       : change.say.isEmpty() ? tr("Changed.") : change.say;
        m_changing = false;
        emit changeChanged();
    });
    connect(m_changeCall.get(), &AiCall::failed, this, [this](const QString &error) {
        m_changeNote = error;
        m_changing = false;
        emit changeChanged();
    });
    m_changeCall->ask(Prompts::change(InterfaceChanges::state(*m_custom, *m_settings, m_colors()), wish));
}

void Librarian::undoChange() {
    if (InterfaceChanges::undo(*m_custom, *m_settings))
        m_changeNote = tr("Put back as it was.");
    emit changeChanged();
}
