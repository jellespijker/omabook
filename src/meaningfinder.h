#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <expected>
#include <optional>
#include <stop_token>

#include "embeddings.h"
#include "failure.h"
#include "meaningindex.h"
#include "meaningsearch.h"

class Reader;

// The open book found by what it means (#72), for the reader's Find: whether
// it is indexed, its indexing beside the window (asked for here, or by
// itself when the book was asked for before and its file changed, or when
// the settings index every book), which can be cancelled, and a search of
// it by the meaning and the words of what was typed, up to the reader's
// place unless `whole`. Everything that waits for the server or the library
// is done on a thread of its own, on a library of its own.
class MeaningFinder : public QObject {
    Q_OBJECT
    // The reader whose open book is looked in (the window's `reader`).
    Q_PROPERTY(QObject *reader READ reader WRITE setReader NOTIFY readerChanged)
    // The window's settings, read again on the thread that looks (else config.json where it always is).
    Q_PROPERTY(QObject *settings READ settings WRITE setSettings NOTIFY readerChanged)
    // "none" (not indexed), "stale" (its file or the model changed), "indexing",
    // "ready", or "refused" when its text may not be sent anywhere (`problem` says why).
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    // Where its text goes to be indexed, said before it is sent; and whether
    // that is someone else's computer (only as the settings allow).
    Q_PROPERTY(QString where READ where NOTIFY changed)
    Q_PROPERTY(bool hosted READ hosted NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    // Why, as a code the pane words what to do by (#172): "no-model", "unreachable",
    // "refused" (its key), "no-key", "hosted-not-allowed", "hosted-not-chosen",
    // "ai-off", "no-server", "failed" (anything else); empty with no problem.
    Q_PROPERTY(QString why READ why NOTIFY changed)
    Q_PROPERTY(bool searching READ searching NOTIFY changed)
    // Past the reader's place too.
    Q_PROPERTY(bool whole MEMBER m_whole NOTIFY wholeChanged)

public:
    explicit MeaningFinder(QObject *parent = nullptr);
    // What is under way is stopped, and waited for: its library is written to.
    ~MeaningFinder() override;
    MeaningFinder(const MeaningFinder &) = delete;
    MeaningFinder &operator=(const MeaningFinder &) = delete;
    MeaningFinder(MeaningFinder &&) = delete;
    MeaningFinder &operator=(MeaningFinder &&) = delete;

    [[nodiscard]] QObject *reader() const;
    void setReader(QObject *reader);
    [[nodiscard]] QString state() const { return m_state; }
    [[nodiscard]] double progress() const { return m_progress; }
    [[nodiscard]] QString where() const { return m_where; }
    [[nodiscard]] bool hosted() const { return m_route && m_route->hosted; }
    [[nodiscard]] QString problem() const { return m_problem; }
    [[nodiscard]] QString why() const { return m_why; }
    [[nodiscard]] QObject *settings() const;
    void setSettings(QObject *settings);
    [[nodiscard]] bool searching() const { return m_search.isRunning(); }

    // Indexes the open book, and keeps it up to date from now on: shown at
    // once, with the settings looked at again first.
    Q_INVOKABLE void index();
    Q_INVOKABLE void cancel();
    // Finds the words by meaning in the open book, once it is indexed: found()
    // then says where, as the Find lists a place: [{section, fraction, title,
    // snippet, meant: true, by}].
    Q_INVOKABLE void find(const QString &words);

signals:
    void readerChanged();
    void changed();
    void wholeChanged();
    void found(const QVariantList &results);

private:
    struct Look {
        std::expected<Embeddings::Route, Failure> route;
        MeaningSearch::Freshness freshness = MeaningSearch::Freshness::None;
        bool due = false; // to be indexed now, without asking
    };
    using Made = std::expected<int, Failure>;
    using Hits = std::expected<QList<MeaningIndex::Hit>, Failure>;

    void look();
    void looked();
    void start();
    void indexed();
    void searched();
    void setState(const QString &state);
    void setProblem(const std::optional<Failure> &failure);
    [[nodiscard]] QString whereIs(int section) const;

    QPointer<Reader> m_reader;
    QString m_state = QStringLiteral("none");
    double m_progress = 0;
    QString m_where, m_problem, m_why;
    QPointer<QObject> m_settings;
    // Why the book's indexing failed, said until it is asked for again.
    std::optional<Failure> m_failed;
    bool m_asked = false; // to be indexed once looked at
    bool m_whole = false;
    std::optional<Embeddings::Route> m_route;
    qint64 m_bookId = 0; // the book looked at, indexed or searched
    // A book whose indexing failed or was cancelled, not begun again by itself.
    qint64 m_leftAlone = 0;
    std::stop_source m_stop;
    QFutureWatcher<Look> m_look;
    QFutureWatcher<Made> m_index;
    QFutureWatcher<Hits> m_search;
};
