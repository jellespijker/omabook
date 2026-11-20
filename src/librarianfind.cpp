// Librarian: finding books by a question, with the AI (aisearch.h).

#include "librarian.h"

#include <QCoreApplication>
#include <QLocale>
#include <QThreadPool>

#include "backend.h"
#include "booklistmodel.h"
#include "settings.h"

QVariantList Librarian::found() const {
    QVariantList list;
    const Library *library = m_backend->library();
    if (!library)
        return list;
    for (const AiSearch::Pick &pick : m_found) {
        const auto book = library->book(pick.bookId);
        // A book removed since it was found is no longer there to open.
        if (!book)
            continue;
        QVariantMap map = bookToMap(*book, library->root());
        map.insert("why", pick.why);
        map.insert("score", pick.score);
        map.insert("readable", !book->formats.isEmpty());
        list << map;
    }
    return list;
}

void Librarian::find(const QString &question) {
    const QString asked = question.trimmed().startsWith('?') ? question.trimmed().mid(1).trimmed() : question.trimmed();
    Library *library = m_backend->library();
    if (!library || asked.isEmpty())
        return;
    stopFinding();
    m_findQuestion = asked;
    m_found.clear();
    m_findError.clear();
    m_findCached = false;
    QString why;
    if (!m_backend->newAi(&why)) {
        m_findError = why;
        emit foundChanged();
        return;
    }
    // The collection shown is what is looked through, as on the shelf.
    const QString key = AiSearch::cacheKey(*library, asked, m_backend->collection());
    if (const auto kept = AiSearch::cached(key)) {
        m_findCached = true;
        finishFinding(*kept, {});
        return;
    }
    m_finding = true;
    const QList<Book> books = m_backend->shownBooks();
    m_findStatus = tr("Looking through %Ln books…", "", int(std::min<qsizetype>(books.size(), AiSearch::mostBooks)));
    emit foundChanged();

    // What the graph knows of the books goes along, read beside the window
    // unless the shelf's search has it to hand already.
    if (m_settings->graph() == "off") {
        startFinding({}, books, key);
        return;
    }
    if (const auto &shelf = m_backend->books()->knowledge(); shelf && shelf->changes == library->changes()) {
        startFinding(shelf, books, key);
        return;
    }
    const int asking = ++m_findAsked;
    const QString root = library->root();
    const QPointer<Librarian> self(this);
    QThreadPool::globalInstance()->start([self, root, asking, before = m_backend->books()->knowledge(), books, key] {
        Library own(root);
        const auto known = std::make_shared<const KnowledgeIndex>(own.open() ? KnowledgeIndex::since(own, before.get()) : KnowledgeIndex());
        QMetaObject::invokeMethod(qApp, [self, asking, known, books, key] {
            if (self && self->m_finding && asking == self->m_findAsked)
                self->startFinding(known, books, key);
        });
    });
}

void Librarian::startFinding(std::shared_ptr<const KnowledgeIndex> known, const QList<Book> &books, const QString &key) {
    auto *run = new AiSearch::Run([backend = m_backend](QString *why) { return backend->newAi(why); }, m_findQuestion, books,
                                  std::move(known), this);
    m_findRun = run;
    const int total = run->books();
    connect(run, &AiSearch::Run::progressed, this, [this, run, total] {
        if (run != m_findRun)
            return;
        m_found = run->picks();
        const int seen = std::min(total, run->done() * AiSearch::batchSize);
        m_findStatus = tr("Looked through %1 of %2 books…").arg(QLocale().toString(seen), QLocale().toString(total));
        emit foundChanged();
    });
    connect(run, &AiSearch::Run::finished, this, [this, run, key] {
        if (run != m_findRun)
            return;
        finishFinding(run->picks(), key);
    });
    connect(run, &AiSearch::Run::failed, this, [this, run](const QString &error) {
        if (run != m_findRun)
            return;
        m_findError = error;
        m_finding = false;
        m_findRun.clear();
        run->deleteLater();
        emit foundChanged();
    });
    run->start();
}

void Librarian::finishFinding(const QList<AiSearch::Pick> &picks, const QString &key) {
    m_found = picks;
    m_finding = false;
    m_findStatus.clear();
    if (!key.isEmpty())
        AiSearch::remember(key, picks);
    if (m_findRun) {
        m_findRun->deleteLater();
        m_findRun.clear();
    }
    emit foundChanged();
}

void Librarian::stopFinding() {
    ++m_findAsked;
    if (m_findRun) {
        m_findRun->cancel();
        m_findRun->deleteLater();
        m_findRun.clear();
    }
    if (!m_finding)
        return;
    m_finding = false;
    m_findStatus.clear();
    emit foundChanged();
}
