#include "bookinkwords.h"

#include <expected>
#include <vector>

#include "bookink.h"
#include "library.h"
#include "log.h"
#include "settings.h"

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

BookInkWords::BookInkWords(RecognitionQueue::Route route, SetUp setUp, RecognitionQueue::LibraryOf library, QObject *parent)
    : QObject(parent), m_queue(std::move(route), library), m_setUp(std::move(setUp)), m_library(std::move(library)) {
    m_rest.setSingleShot(true);
    m_rest.setInterval(4s);
    connect(&m_rest, &QTimer::timeout, this, &BookInkWords::readWaiting);
}

BookInkWords::BookInkWords(const Settings &settings, RecognitionQueue::LibraryOf library, QObject *parent)
    : BookInkWords(RecognitionQueue::routeFor(settings), [&settings] { return setUp(settings); }, std::move(library), parent) {}

bool BookInkWords::setUp(const Settings &settings) { return !settings.recognition().value("ink_model"_L1).toString().trimmed().isEmpty(); }

void BookInkWords::setRest(std::chrono::milliseconds rest) { m_rest.setInterval(rest); }

void BookInkWords::changed(qint64 bookId, int page) {
    if (bookId <= 0 || page < 0 || !m_setUp || !m_setUp())
        return;
    m_waiting.insert({bookId, page});
    m_rest.start();
}

void BookInkWords::readWaiting() {
    const QSet<std::pair<qint64, int>> waiting = std::exchange(m_waiting, {});
    const Library *library = m_library ? m_library() : nullptr;
    if (library == nullptr)
        return;
    for (const auto &[bookId, page] : waiting) {
        std::vector<Ink::Stroke> strokes = BookInk::strokes(*library, bookId, page);
        // A page erased has no ink, and nothing to keep its words on.
        if (strokes.empty())
            continue;
        // Kept on the ink itself, not with a plugin's results; never to a hosted server.
        RecognitionQueue::Request request{
            .input = {.kind = Recognition::Kind::Ink, .strokes = std::move(strokes), .file = {}, .language = {}},
            .owner = {},
            .hosted = false,
            .again = false};
        m_queue.submit(std::move(request), [this, bookId, page](const std::expected<Recognition::Result, QString> &outcome) {
            Library *now = m_library ? m_library() : nullptr;
            if (!outcome) {
                qCInfo(lcAi).noquote() << "The ink on page" << page + 1 << "of book" << bookId << "wasn't read:" << outcome.error();
                return;
            }
            if (now != nullptr && BookInk::setWords(*now, bookId, page, outcome->text))
                emit read(bookId, page, outcome->text.simplified());
        });
    }
}
