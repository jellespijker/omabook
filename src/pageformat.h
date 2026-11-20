#pragma once

#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include "pagelayout.h"

class Reader;
class Settings;
namespace Hyphenation {
class Dictionary;
}

// The reading format for the window: what is in force for the open book,
// the defaults and the book's own, and the chapter set with them. It
// changes them too, as the "Aa" panel and the settings ask.
class PageFormat : public QObject {
    Q_OBJECT
    // The options in force, by key: the defaults with the open book's own over them.
    Q_PROPERTY(QVariantMap effective READ effective NOTIFY changed)
    Q_PROPERTY(QVariantMap defaults READ defaults NOTIFY changed)
    // Only the open book's own options; empty when it follows the defaults.
    Q_PROPERTY(QVariantMap own READ own NOTIFY changed)
    // Whether a book is open for options of its own to be given to.
    Q_PROPERTY(bool hasBook READ hasBook NOTIFY changed)
    // The collection (library.h) the open book is set with, or would be
    // given options for: the one whose format is in force, else the one the
    // book was opened from when it is in it (`preferredCollection`), else
    // the first it is in. Its number, its name and its own options; 0 and
    // empty for a book in no collection. Its options lie over the defaults,
    // and the book's own over them.
    Q_PROPERTY(qint64 collectionId READ collectionId NOTIFY changed)
    Q_PROPERTY(QString collectionName READ collectionName NOTIFY changed)
    Q_PROPERTY(QVariantMap collectionOwn READ collectionOwn NOTIFY changed)
    Q_PROPERTY(qint64 preferredCollection READ preferredCollection WRITE setPreferredCollection NOTIFY changed)
    Q_PROPERTY(QVariantMap bounds READ bounds CONSTANT)
    Q_PROPERTY(QStringList fonts READ fonts CONSTANT)
    // The chapter in front of the reader, set with the options in force.
    Q_PROPERTY(QString html READ html NOTIFY htmlChanged)
    // The color links are shown in, which is the theme's.
    Q_PROPERTY(QColor linkColor READ linkColor WRITE setLinkColor NOTIFY htmlChanged)
    // The language justified text is hyphenated by, named in itself
    // ("English", "Deutsch"); empty when no dictionary for the open book's
    // language is installed, or no book is open.
    Q_PROPERTY(QString hyphenationLanguage READ hyphenationLanguage NOTIFY changed)

    // How the text is laid out across the room it has (pagelayout.h). The
    // reading view says how wide its reading area is (`available`, the
    // panes beside it already taken off); the rest follows from it, the
    // format in force and the setting of pages, and every reader lays its
    // text out by these and nothing else.
    Q_PROPERTY(double available READ available WRITE setAvailable NOTIFY layoutChanged)
    // The advance of an average character of the body font, and the longest
    // line (the measure) in pixels.
    Q_PROPERTY(double characterWidth READ characterWidth NOTIFY layoutChanged)
    Q_PROPERTY(double lineWidth READ lineWidth NOTIFY layoutChanged)
    // The room at the spread's outer edges, and between two of its pages.
    Q_PROPERTY(double margin READ margin NOTIFY layoutChanged)
    Q_PROPERTY(double gutter READ gutter NOTIFY layoutChanged)
    // The pages side by side (one for a book that scrolls); what Automatic gives here; the most that keep
    // the minimum measure; each page's width; and whether a choice of 1, 2
    // or 3 is what shows.
    Q_PROPERTY(int columns READ columns NOTIFY layoutChanged)
    Q_PROPERTY(int automaticColumns READ automaticColumns NOTIFY layoutChanged)
    Q_PROPERTY(int mostColumns READ mostColumns NOTIFY layoutChanged)
    Q_PROPERTY(double columnWidth READ columnWidth NOTIFY layoutChanged)
    Q_PROPERTY(bool pagesHonoured READ pagesHonoured NOTIFY layoutChanged)
    // The fewest characters a page of a spread is set to.
    Q_PROPERTY(int minimumMeasure READ minimumMeasure CONSTANT)

public:
    PageFormat(Reader *reader, Settings *settings, QObject *parent = nullptr);

    QVariantMap effective() const { return m_effective.toVariantMap(); }
    QVariantMap defaults() const { return m_defaults.toVariantMap(); }
    QVariantMap own() const { return m_own.toVariantMap(); }
    bool hasBook() const;
    qint64 collectionId() const noexcept { return m_collectionId; }
    QString collectionName() const noexcept { return m_collectionName; }
    QVariantMap collectionOwn() const { return m_collectionOwn.toVariantMap(); }
    qint64 preferredCollection() const noexcept { return m_preferred; }
    void setPreferredCollection(qint64 id);
    QVariantMap bounds() const;
    // The fonts there are, by family, for the font picker.
    QStringList fonts() const;
    QString html() const noexcept { return m_html; }
    QColor linkColor() const noexcept { return m_links; }
    void setLinkColor(const QColor &color);
    QString hyphenationLanguage() const;

    double available() const noexcept { return m_available; }
    void setAvailable(double width);
    double characterWidth() const noexcept { return m_space.characterWidth; }
    double lineWidth() const noexcept { return m_space.characterWidth * m_space.measure; }
    double margin() const noexcept { return m_space.margin; }
    double gutter() const noexcept { return m_space.gutter; }
    int columns() const noexcept { return m_columns.count; }
    int automaticColumns() const noexcept { return m_columns.automatic; }
    int mostColumns() const noexcept { return m_columns.most; }
    double columnWidth() const noexcept { return m_columns.width; }
    bool pagesHonoured() const noexcept { return m_columns.honoured; }
    static constexpr int minimumMeasure() noexcept { return PageLayout::minimumMeasure; }

    // Changes one option, for the open book alone or for the defaults; what
    // is wrong with the value when it can't be, else empty.
    Q_INVOKABLE QString set(const QString &key, const QVariant &value, bool forBook);
    // The same, for "book", "collection" (the book's collection, above) or
    // "defaults".
    Q_INVOKABLE QString setFor(const QString &key, const QVariant &value, const QString &where);
    // The open book follows the defaults again.
    Q_INVOKABLE void resetBook();
    // The book's collection follows the defaults again.
    Q_INVOKABLE void resetCollection();
    // The defaults and the book's own as they are, and back to that: what
    // the panel's Reset puts back.
    Q_INVOKABLE QVariantMap snapshot() const;
    Q_INVOKABLE void restore(const QVariantMap &snapshot);
    // Larger or smaller type, where the size is: the book's own size when
    // it has one, else the default.
    Q_INVOKABLE void zoom(int steps);
    // The next layout (readingformat.h: auto, pages, scroll, and round
    // again), where the layout in force comes from; the one it is now.
    Q_INVOKABLE QString nextLayout();

    // Reads the defaults and the open book's own again.
    void refresh();

signals:
    void changed();
    void htmlChanged();
    void layoutChanged();

private:
    void restyle();
    // Takes the open book's language and its dictionary, when they changed.
    bool followLanguage();
    // Where the option in force comes from: "book", "collection" or "defaults".
    QString from(const QString &key) const;
    // Works the layout out again from the room, the format and the setting.
    void relayout();

    Reader *m_reader;
    Settings *m_settings;
    QJsonObject m_defaults, m_own, m_effective, m_collectionOwn;
    qint64 m_collectionId = 0, m_preferred = 0;
    QString m_collectionName;
    QString m_html, m_chapter;
    // The open book's language, and the dictionary for it (hyphenation.h).
    QString m_language;
    const Hyphenation::Dictionary *m_dictionary = nullptr;
    QColor m_links{"#7aa2f7"};
    double m_available = 0;
    PageLayout::Space m_space;
    PageLayout::Columns m_columns;
};
