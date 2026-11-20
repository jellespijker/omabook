#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <expected>

#include "annotationanchors.h"

class Reader;

// The open book's highlights and margin notes found again in the file it is
// read from (#80), when they were placed in another: a new edition, a typo
// fixed, another format. It happens beside the window, on a library of its
// own, each time a book is opened or its file changes under the reader, and
// only when one of its passages isn't placed in that file yet, which is one
// question to the library.
class ReadingPassages : public QObject {
    Q_OBJECT

public:
    explicit ReadingPassages(Reader *reader, QObject *parent = nullptr);
    // What is under way is finished first: its library is written to.
    ~ReadingPassages() override { m_work.waitForFinished(); }
    ReadingPassages(const ReadingPassages &) = delete;
    ReadingPassages &operator=(const ReadingPassages &) = delete;
    ReadingPassages(ReadingPassages &&) = delete;
    ReadingPassages &operator=(ReadingPassages &&) = delete;
    // Whether passages are being found again now.
    [[nodiscard]] bool working() const { return m_work.isRunning(); }

signals:
    // A book's passages were found again: so many moved, so many are lost.
    void anchored(qint64 bookId, int moved, int lost);

private:
    using Outcome = std::expected<AnnotationAnchors::Summary, QString>;
    void follow();
    void done();

    Reader *m_reader;
    QFutureWatcher<Outcome> m_work;
    qint64 m_working = 0;
};
