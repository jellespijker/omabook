#include "readingstatsview.h"

#include <QDate>
#include <QtConcurrent>

#include "backend.h"
#include "highlightcolors.h"
#include "library.h"
#include "log.h"
#include "readinggoal.h"

using namespace Qt::StringLiterals;

ReadingStatsView::ReadingStatsView(QObject *parent) : QObject(parent), m_year(QDate::currentDate().year()) {
    m_soon.setSingleShot(true);
    m_soon.setInterval(300);
    connect(&m_soon, &QTimer::timeout, this, &ReadingStatsView::count);
    connect(&m_work, &QFutureWatcher<Outcome>::finished, this, &ReadingStatsView::done);
}

QObject *ReadingStatsView::backend() const { return m_backend; }

void ReadingStatsView::setBackend(QObject *backend) {
    auto *given = qobject_cast<Backend *>(backend);
    if (given == m_backend)
        return;
    if (m_backend)
        m_backend->disconnect(this);
    m_backend = given;
    if (m_backend) {
        // Books read, finished or changed, here or by a sync; another library.
        connect(m_backend, &Backend::revisionChanged, this, &ReadingStatsView::refresh);
        connect(m_backend, &Backend::libraryChanged, this, &ReadingStatsView::refresh);
        connect(m_backend, &Backend::readingGoalChanged, this, &ReadingStatsView::refresh);
        connect(m_backend, &QObject::destroyed, this, [this] { m_backend = nullptr; });
    }
    emit backendChanged();
    refresh();
}

void ReadingStatsView::setYear(int year) {
    if (year == m_year || year < 1 || year > 9999)
        return;
    m_year = year;
    emit askedChanged();
    refresh();
}

void ReadingStatsView::setCollection(const QString &collection) {
    if (collection == m_collection)
        return;
    m_collection = collection;
    emit askedChanged();
    refresh();
}

void ReadingStatsView::setGoal(const QString &kind, int target) {
    const Library *library = m_backend ? m_backend->library() : nullptr;
    const auto named = ReadingGoal::kindNamed(kind);
    if (!library || !named)
        return;
    if (const auto set = ReadingGoal::set(*library, {.year = m_year, .kind = *named, .target = std::max(0, target)}); !set) {
        m_backend->say(set.error());
        return;
    }
    emit m_backend->readingGoalChanged();
}

QColor ReadingStatsView::mark(const QColor &color, const QColor &background) const {
    // Towards white on a dark background, towards black on a light one.
    const bool dark = background.lightnessF() < 0.5;
    QColor shown = color;
    for (int step = 0; step < 20 && HighlightColors::contrast(shown, background) < 3; ++step)
        shown = dark ? shown.lighter(110) : shown.darker(110);
    // A colour with no light in it to lift: the text's own, which always stands out.
    if (HighlightColors::contrast(shown, background) < 3)
        return dark ? QColor(Qt::white) : QColor(Qt::black);
    return shown;
}

void ReadingStatsView::setActive(bool active) {
    if (active == m_active)
        return;
    m_active = active;
    emit activeChanged();
    if (m_active && m_stale)
        refresh();
}

void ReadingStatsView::refresh() {
    // While nothing shows it, it only remembers that it is behind.
    m_stale = !m_active;
    if (m_active)
        m_soon.start();
}

void ReadingStatsView::count() {
    const Library *library = m_backend ? m_backend->library() : nullptr;
    if (!library)
        return;
    if (m_work.isRunning()) {
        m_again = true;
        return;
    }
    m_work.setFuture(QtConcurrent::run([root = library->root(), year = m_year, collection = m_collection] -> Outcome {
        Library own(root);
        QString why;
        if (!own.open(&why))
            return std::unexpected(why);
        return ReadingStats::of(own, year, collection);
    }));
    emit workingChanged();
}

void ReadingStatsView::done() {
    emit workingChanged();
    if (std::exchange(m_again, false)) {
        count();
        return;
    }
    const Outcome outcome = m_work.result();
    if (!outcome) {
        qCWarning(lcLibrary).noquote() << "the reading statistics can't be counted:" << outcome.error();
        return;
    }
    m_summary = ReadingStats::toJson(*outcome).toVariantMap();
    // The goal is the whole library's, whatever collection is shown: done is counted of every book.
    m_goal = m_collection.isEmpty() ? m_summary.value(u"goal"_s).toMap() : m_goal;
    emit changed();
}
