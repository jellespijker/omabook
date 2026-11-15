// The window's side of collections (library.h): which one the shelf shows,
// how each is shown, and making, changing and filling them. Kept apart from
// the rest of Backend, which it only touches through the shelf.

#include "backend.h"

#include <QJsonObject>

namespace {

// A value as one word of a search: quoted when it has spaces in it.
QString searchValue(const QString &value) {
    QString plain = value;
    plain.remove('"');
    return plain.contains(' ') ? '"' + plain + '"' : plain;
}

} // namespace

QString Backend::collectionName() const {
    for (const QVariant &value : m_collections)
        if (value.toMap().value("id").toLongLong() == m_collection)
            return value.toMap().value("path").toString();
    return {};
}

QList<Book> Backend::shownBooks() const {
    if (!m_library)
        return {};
    const QList<Book> books = m_library->books();
    const auto collection = m_collection ? m_library->collection(m_collection) : std::nullopt;
    return collection ? collection->of(books) : books;
}

void Backend::readCollections(const QList<Book> &books) {
    const QList<Collection> collections = m_library ? m_library->collections() : QList<Collection>();
    QVariantList list;
    std::optional<QSet<qint64>> members;
    for (const Collection &collection : collections) {
        QSet<qint64> in;
        for (const Book &book : books)
            if (collection.contains(book))
                in.insert(book.id);
        list << QVariantMap{{"id", collection.id},
                            {"name", collection.name},
                            {"path", collection.path},
                            {"parent", collection.parent},
                            {"depth", collection.depth},
                            {"children", collection.hasChildren},
                            {"rule", collection.rule},
                            {"count", int(in.size())},
                            {"included", int(collection.included.size())},
                            {"excluded", int(collection.excluded.size())},
                            {"format", !collection.format.isEmpty()}};
        if (collection.id == m_collection)
            members = in;
    }
    // The collection in view went (removed here or elsewhere): all the books.
    const bool gone = m_collection && !members;
    if (gone)
        m_collection = 0;
    m_model.setMembers(members);
    if (list != m_collections || gone) {
        m_collections = list;
        emit collectionsChanged();
    }
    if (gone) {
        showCollectionShelf();
        keepShelf();
    }
}

void Backend::setCollection(qint64 id) {
    if (id == m_collection || !m_library || (id != 0 && !m_library->collection(id)))
        return;
    m_collection = id;
    readCollections(m_library->books());
    emit collectionsChanged();
    showCollectionShelf();
    keepShelf();
}

void Backend::stepCollection(int by) {
    QList<qint64> ids{0};
    for (const QVariant &value : std::as_const(m_collections))
        ids << value.toMap().value("id").toLongLong();
    const qsizetype at = std::max<qsizetype>(0, ids.indexOf(m_collection));
    setCollection(ids.at(((at + by) % ids.size() + ids.size()) % ids.size()));
}

void Backend::showCollectionShelf() {
    QString view = m_libraryView, group = m_libraryGroup, sort = m_settings->sort();
    bool reversed = m_settings->sortReversed();
    // A collection that has never been given a view of its own starts as
    // the library is shown, and keeps what it is given from then on.
    if (const auto collection = m_collection && m_library ? m_library->collection(m_collection) : std::nullopt) {
        const QJsonObject shelf = collection->shelf;
        if (views().contains(shelf.value("view").toString()))
            view = shelf.value("view").toString();
        if (groupings().contains(shelf.value("group").toString()))
            group = shelf.value("group").toString();
        if (BookListModel::sorts().contains(shelf.value("sort").toString())) {
            sort = shelf.value("sort").toString();
            reversed = shelf.value("reversed").toBool();
        }
    }
    m_shelfMoving = true;
    m_model.setSort(sort);
    m_model.setReversed(reversed);
    m_shelfMoving = false;
    if (view == m_view && group == m_group)
        return;
    m_view = view;
    m_group = group;
    emit shelfChanged();
}

void Backend::keepCollectionShelf() {
    if (!m_library || !m_collection)
        return;
    m_library->setCollectionShelf(m_collection, {{"view", m_view}, {"group", m_group}, {"sort", m_model.sort()},
                                                 {"reversed", m_model.reversed()}});
}

qint64 Backend::saveCollection(qint64 id, const QString &name, const QString &rule, qint64 parent) {
    if (!m_library)
        return 0;
    QString error;
    if (id == 0) {
        const auto made = m_library->addCollection(name, rule, parent);
        if (!made) {
            say(made.error());
            return 0;
        }
        id = *made;
    } else if (!m_library->setCollectionParent(id, parent, &error) || !m_library->renameCollection(id, name, &error)
               || !m_library->setCollectionRule(id, rule, &error)) {
        say(error);
        return 0;
    }
    reload();
    return id;
}

void Backend::removeCollection(qint64 id) {
    if (!m_library || !m_library->removeCollection(id))
        return;
    reload();
}

int Backend::ruleCount(const QString &rule) const {
    if (!m_library || rule.trimmed().isEmpty())
        return 0;
    const QList<Book> books = m_library->books();
    return int(std::ranges::count_if(books, [&rule](const Book &book) { return book.matches(rule); }));
}

QString Backend::ruleProblem(const QString &rule) const { return Book::searchProblem(rule); }

QString Backend::shelfRule() const {
    QStringList words;
    if (!m_model.query().trimmed().isEmpty())
        words << m_model.query().trimmed();
    const QVariantMap filters = m_model.filters();
    // In the order the filters are listed in; "-" (books that have none of
    // something) has no word in a search and is left out.
    for (const QString &kind : BookListModel::filterKinds()) {
        const QString value = filters.value(kind).toString();
        if (value.isEmpty() || value == "-")
            continue;
        if (kind == "status")
            words << "status:" + value;
        else if (kind == "held")
            words << (value == "digital" ? QStringList{"is:digital"} : value == "paper" ? QStringList{"is:paper"}
                      : value == "both" ? QStringList{"is:digital", "is:paper"} : QStringList{"is:wishlist"});
        else if (kind == "language")
            words << "lang:" + value;
        else
            words << kind + ":" + searchValue(value);
    }
    return words.join(' ');
}

QVariantList Backend::bookCollections(qint64 bookId) const {
    QVariantList list;
    const auto book = m_library && bookId > 0 ? m_library->book(bookId) : std::nullopt;
    if (!book)
        return list;
    for (const Collection &collection : m_library->collections())
        list << QVariantMap{{"id", collection.id},
                            {"name", collection.path},
                            {"in", collection.contains(*book)},
                            {"byHand", collection.included.contains(bookId) || collection.excluded.contains(bookId)}};
    return list;
}

void Backend::setInCollection(qint64 collectionId, qint64 bookId, bool in) {
    if (!m_library || !m_library->putInCollection(collectionId, bookId, in))
        return;
    reload();
}

void Backend::putInShownCollection(const QList<qint64> &ids) {
    const auto collection = m_library && m_collection ? m_library->collection(m_collection) : std::nullopt;
    if (!collection)
        return;
    const Library::Batch batch(*m_library);
    for (const qint64 id : ids)
        if (const auto book = m_library->book(id); book && !collection->contains(*book))
            m_library->putInCollection(collection->id, id, true);
}
