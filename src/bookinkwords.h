#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <chrono>
#include <functional>
#include <utility>

#include "recognitionqueue.h"

class Settings;

// Book ink found by its words (#115): the handwriting on a page of a PDF or a
// comic, once the pen has rested, recognised (#84) and its words kept as the
// text of the page's ink (BookInk::setWords). A search then finds the page by
// them (AnnotationWords), and they go to one's other computers with the ink:
// only the computer the ink was written on has it read.
//
// Only when recognition is set up for handwriting (a model chosen to read it,
// the settings' "ink_model", or a server of its own, "ink_url"), and only by
// the person's own servers (this computer, the tailnet, one they marked as
// theirs): book ink is never sent to a hosted one, whatever the settings
// allow for a plugin that asks.
class BookInkWords : public QObject {
    Q_OBJECT

public:
    using SetUp = std::function<bool()>;

    // The ways `route` gives, asked only while `setUp` says recognition is set
    // up; the words kept in the library `library` answers.
    BookInkWords(RecognitionQueue::Route route, SetUp setUp, RecognitionQueue::LibraryOf library, QObject *parent = nullptr);
    // As these settings say, which must outlive it.
    BookInkWords(const Settings &settings, RecognitionQueue::LibraryOf library, QObject *parent = nullptr);

    // Whether the settings choose a model to read handwriting with.
    [[nodiscard]] static bool setUp(const Settings &settings);

    // The ink on a page of a book changed: its words are read again once the
    // pen has rested this long since the last change of any page.
    void changed(qint64 bookId, int page);
    void setRest(std::chrono::milliseconds rest);
    [[nodiscard]] qsizetype pending() const { return m_waiting.size() + m_queue.pending(); }

signals:
    // A page's words were read and kept.
    void read(qint64 bookId, int page, const QString &words);

private:
    void readWaiting();

    RecognitionQueue m_queue;
    SetUp m_setUp;
    RecognitionQueue::LibraryOf m_library;
    QSet<std::pair<qint64, int>> m_waiting; // by book and page
    QTimer m_rest;
};
