#pragma once

#include <QAbstractListModel>
#include <QColor>
#include <QHash>
#include <QVariantList>
#include <QVariantMap>

#include "library.h"

// A book as QML sees it: the model's roles and Backend::book share this, so
// a field has one name everywhere in the window.
QVariantMap bookToMap(const Book &book, const QString &libraryRoot);

// The shelf: the library's books, filtered by the search and the filters,
// and sorted. Every way of looking at the library (covers, spines, a
// carousel, a list, stacks) shows these rows in this order.
class BookListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    // "recent", "title", "author" (by last name), "series", "added",
    // "progress", "rating", "published" or "finished". Each has the order it
    // is most wanted in (titles from A, ratings from the best); `reversed`
    // turns that round, and `descending` says which way it ended up.
    Q_PROPERTY(QString sort READ sort WRITE setSort NOTIFY sortChanged)
    Q_PROPERTY(bool reversed READ reversed WRITE setReversed NOTIFY sortChanged)
    Q_PROPERTY(bool descending READ descending NOTIFY sortChanged)
    // What narrows the shelf besides the search, one value per kind:
    // {"status": "reading", "held": "paper", "format": "epub", "language":
    // "en", "tag": "Fantasy", "author": "Ann Author", "series": "Dune",
    // "rating": 4}. A book shows when it passes all of them. "-" for a tag,
    // author, series or language means books that have none.
    Q_PROPERTY(QVariantMap filters READ filters WRITE setFilters NOTIFY filtersChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int total READ total NOTIFY countChanged)

public:
    explicit BookListModel(QObject *parent = nullptr);
    ~BookListModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setBooks(const QList<Book> &books, const QString &libraryRoot);

    QString query() const { return m_query; }
    void setQuery(const QString &query);
    QString sort() const { return m_sort; }
    void setSort(const QString &sort);
    bool reversed() const { return m_reversed; }
    void setReversed(bool reversed);
    bool descending() const;
    QVariantMap filters() const { return m_filters; }
    void setFilters(const QVariantMap &filters);
    int count() const { return m_rows.size(); }
    int total() const { return m_books.size(); }

    static QStringList sorts();
    static QStringList filterKinds();

    Q_INVOKABLE qint64 idAt(int row) const;
    Q_INVOKABLE int rowOf(qint64 id) const;
    // Everything about the book in a row, for views that lay books out
    // themselves; empty when there is no such row.
    Q_INVOKABLE QVariantMap get(int row) const;

    // Sets one filter; an empty value, or a rating of 0, takes it away.
    Q_INVOKABLE void setFilter(const QString &kind, const QVariant &value);
    Q_INVOKABLE void clearFilters();
    // What a filter of this kind can be set to in this library, the most
    // common first: [{"value": "Fantasy", "count": 12}].
    Q_INVOKABLE QVariantList choices(const QString &kind) const;
    // The shown books in piles by "series", "author", "tag" or "status",
    // leaving a filter of that same kind aside: [{"value": "Dune", "count":
    // 6, "books": [the first three, as {"cover", "title", "authors"}]}].
    // Books with nothing to go by are the last pile, with the value "-".
    Q_INVOKABLE QVariantList groups(const QString &kind) const;

    // The shown books as spines on shelves `width` wide, for books `height`
    // tall: a list of shelves, each a list of {"row", "x", "y", "w", "h",
    // "flat"}, where x runs from the shelf's start and y up from its board.
    // A spine is as thick as the book is long where that is known, and a
    // few books lie flat in piles between the standing ones; which do is
    // decided by the book alone, so a shelf looks the same every time.
    Q_INVOKABLE QVariantList shelve(qreal width, qreal height) const;
    // A color text reads in on `color`: nearly black or nearly white, with
    // a little of the color's own hue.
    Q_INVOKABLE QColor inkOn(const QColor &color) const;

    // The main color of a picture, as a spine would be bound in.
    static QColor dominantColor(const QImage &image);

signals:
    void queryChanged();
    void sortChanged();
    void filtersChanged();
    void countChanged();

private:
    void rebuild();
    // The books that pass the search and every filter but one of `except`'s
    // kind, as indexes into m_books, in the shelf's order.
    QList<int> arranged(const QString &except = {}) const;
    bool passes(const Book &book, const QString &kind, const QVariant &value) const;
    QColor spineColor(int book) const;
    double thickness(int book) const;
    void loadColors() const;

    QList<Book> m_books;
    // Each book as QML sees it, made once when the books arrive: a search
    // or another order only picks among them.
    QList<QVariantMap> m_maps;
    QString m_root;
    QString m_query;
    QString m_sort = "recent";
    bool m_reversed = false;
    QVariantMap m_filters;
    QList<int> m_rows;

    // A cover's color by the cover's address, kept between runs, since
    // finding it means opening the picture.
    mutable QHash<QString, QColor> m_colors;
    mutable bool m_colorsLoaded = false;
    mutable bool m_colorsChanged = false;
    mutable QHash<qint64, double> m_thickness;
};
