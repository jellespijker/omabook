#include "readingpassages.h"

#include <QtConcurrent>

#include "library.h"
#include "log.h"
#include "reader.h"

ReadingPassages::ReadingPassages(Reader *reader, QObject *parent) : QObject(parent), m_reader(reader) {
    connect(reader, &Reader::bookChanged, this, &ReadingPassages::follow);
    connect(&m_work, &QFutureWatcher<Outcome>::finished, this, &ReadingPassages::done);
}

void ReadingPassages::follow() {
    const Library *library = m_reader->library();
    const qint64 bookId = m_reader->bookId();
    // One at a time; the book open once it is done is looked at then.
    if (!library || bookId <= 0 || m_work.isRunning())
        return;
    const std::optional<FormatFile> file = AnnotationAnchors::readableFile(*library, bookId);
    if (!file || !AnnotationAnchors::needed(*library, bookId, file->hash))
        return;
    m_working = bookId;
    m_work.setFuture(QtConcurrent::run([root = library->root(), bookId] -> Outcome {
        Library own(root);
        QString why;
        if (!own.open(&why))
            return std::unexpected(why);
        return AnnotationAnchors::anchor(own, bookId);
    }));
}

void ReadingPassages::done() {
    const qint64 bookId = std::exchange(m_working, 0);
    if (const Outcome outcome = m_work.result(); outcome)
        emit anchored(bookId, outcome->moved, outcome->lost);
    else
        qCWarning(lcLibrary).noquote() << "the passages of book" << bookId << "can't be found again:" << outcome.error();
    if (m_reader->bookId() != bookId)
        follow();
}
