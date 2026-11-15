#pragma once

#include <QColor>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <map>
#include <memory>

#include "graph.h"
#include "textmarks.h"

class Reader;
class QTextDocument;

// The things of the open book's knowledge graph, marked in the chapter on
// the page: each name the graph knows, up to the reader's place, gets a
// quiet tint and underline in its kind's color, the color its disc has in
// the graph. Marking only changes how characters are drawn (an underline, a
// background), never their size, so the page is not laid out again and the
// reading place doesn't move.
//
// It is lazy and kept. A chapter's marks are worked out off the window's
// thread, once per (book, chapter, its text, the names known), and taken
// from the cache when the chapter is shown again. The names known change
// only when the graph changed for this book (the library's change counter
// says whether it changed at all, and the book's own things are compared)
// or when the reader passed the place where something new was met; turning
// pages within what is known costs one comparison.
class ReadingMarks : public QObject {
    Q_OBJECT
    // The theme, for the kinds' colors: the same three as the graph's.
    Q_PROPERTY(QVariantMap colors READ colors WRITE setColors NOTIFY lookChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY lookChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY lookChanged)
    // How many things can be marked where the reader is.
    Q_PROPERTY(int known READ known NOTIFY changed)

public:
    explicit ReadingMarks(Reader *reader, QObject *parent = nullptr);
    ~ReadingMarks() override;
    // How the knowledge graph is kept (see Assistant::setGraphMode): with
    // "off" nothing is read from it, so nothing is marked.
    void setGraphMode(std::function<QString()> mode) { m_graphMode = std::move(mode); }

    QVariantMap colors() const noexcept { return m_colors; }
    void setColors(const QVariantMap &colors);
    QColor foreground() const noexcept { return m_foreground; }
    void setForeground(const QColor &color);
    QColor background() const noexcept { return m_background; }
    void setBackground(const QColor &color);
    int known() const noexcept { return int(m_things.size()); }

    // Lays the marks on the chapter a TextEdit shows: at once from the
    // cache, else once they are worked out. Nothing when they are on already.
    Q_INVOKABLE void mark(QQuickTextDocument *chapter);
    // Takes them off again.
    Q_INVOKABLE void unmark(QQuickTextDocument *chapter);
    // The thing marked at a character position of a chapter, with where its
    // mark starts and ends ({start, end}); empty where nothing is marked.
    Q_INVOKABLE QVariantMap at(QQuickTextDocument *chapter, int position) const;
    // Everything the reader may know of a thing: {id, kind, name, summary,
    // aliases, color, met: {section, fraction, title}, relations: [{relation,
    // other, otherId, outgoing}]}; empty for one not met yet.
    Q_INVOKABLE QVariantMap thing(qint64 id) const;
    // The known thing that selected words are a name of, or empty.
    Q_INVOKABLE QVariantMap named(const QString &words) const;
    // Questions worth asking about a thing (or, with id 0, about selected
    // words): those made from the graph, then any the AI suggested for the
    // same thing at the same place. [{text, ai}]
    Q_INVOKABLE QVariantList suggestions(qint64 id, const QString &words = {});
    // The AI's suggestions are kept by (book, thing or words, chapter).
    Q_INVOKABLE QString suggestionKey(qint64 id, const QString &words) const;
    Q_INVOKABLE bool suggested(const QString &key) const { return m_suggested.contains(key); }
    void keepSuggestions(const QString &key, const QStringList &questions);
    // A kind's color, and the tint its marks lie on.
    Q_INVOKABLE QColor colorOf(const QString &kind) const;
    Q_INVOKABLE QColor tintOf(const QString &kind) const;
    // An answer of the AI (Markdown) as rich text with the known things in
    // it marked as they are on the page, each a link "omabook-thing:<id>".
    // Empty when it names none, so the answer is shown as it was.
    Q_INVOKABLE QString linked(const QString &markdown) const;
    // The paragraph around a position, the word at one, and the start of the
    // word `steps` words on (back, with a negative count).
    Q_INVOKABLE QString paragraph(QQuickTextDocument *chapter, int position) const;
    Q_INVOKABLE QVariantMap wordAt(QQuickTextDocument *chapter, int position) const;
    Q_INVOKABLE int wordFrom(QQuickTextDocument *chapter, int position, int steps) const;
    // When the graph has noted the book less far than the reader has read,
    // by more than is worth a request: how far, as "Noted up to <chapter>".
    // Empty when it is up to date or there is no graph.
    Q_INVOKABLE QString notedUpTo();
    // Reads the graph again if it changed (the AI noted what was read).
    Q_INVOKABLE void refresh();

    // What is marked in a text of a chapter of the open book, from the
    // cache or worked out now, on this thread; for tests and the timings.
    QList<TextMarks::Mark> marksIn(int section, const QString &text);
    struct Counts {
        int hits = 0, misses = 0, loads = 0;
    };
    Counts counts() const noexcept { return m_counts; }

signals:
    // What is known changed: another book, the graph grew, the reader met
    // something new. Every chapter shown is marked again.
    void changed();
    void lookChanged();
    // Marks were laid on a chapter.
    void marked();
    void suggestionsChanged();

private:
    struct Key {
        qint64 book = 0;
        int section = 0;
        size_t text = 0, names = 0;
        bool operator==(const Key &) const = default;
        auto operator<=>(const Key &) const = default;
        friend size_t qHash(const Key &key, size_t seed = 0) { return qHashMulti(seed, key.book, key.section, key.text, key.names); }
    };
    using Marks = QList<TextMarks::Mark>;
    bool enabled() const;
    void load();
    void follow();
    void rebuild(bool force);
    Key keyOf(int section, const QString &text) const;
    void keep(const Key &key, const Marks &marks);
    void apply(QTextDocument *document, const Marks &marks, const QString &stamp) const;
    void clear(QTextDocument *document) const;
    // Whether the marks are still in the text: setting a page's text anew,
    // as a change of its format does, takes them out.
    bool stillMarked(QTextDocument *document, const Marks &marks) const;
    QString stampOf(const Key &key) const;
    QString titleOf(int section) const;

    Reader *m_reader;
    std::function<QString()> m_graphMode;
    QVariantMap m_colors;
    QColor m_foreground{Qt::white}, m_background{Qt::black};
    int m_look = 0;
    // What the open book's reader has met, in order, and the graph's change
    // count when that was read.
    qint64 m_book = 0;
    qint64 m_graphCount = -1;
    QList<std::pair<GraphNode, GraphPlace>> m_met;
    qsizetype m_reach = -1;
    QHash<qint64, TextMarks::Thing> m_things;
    std::shared_ptr<const TextMarks::Matcher> m_matcher;
    QHash<Key, Marks> m_cache;
    QList<Key> m_order; // oldest first, so the cache stays small
    std::map<Key, std::unique_ptr<QFutureWatcher<Marks>>> m_working;
    QHash<Key, QList<QPointer<QQuickTextDocument>>> m_waiting;
    QHash<QString, QStringList> m_suggested;
    QHash<QString, QVariantList> m_madeSuggestions;
    QString m_noted;
    QString m_notedFor;
    Counts m_counts;
};
