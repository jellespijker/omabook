#include "bookmeta.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <utility>

namespace {

[[nodiscard]] QStringList splitList(const QString &text, const QRegularExpression &separator) {
    QStringList parts;
    for (const QString &part : text.split(separator, Qt::SkipEmptyParts)) {
        const QString trimmed = part.simplified();
        if (!trimmed.isEmpty() && !parts.contains(trimmed, Qt::CaseInsensitive))
            parts << trimmed;
    }
    return parts;
}

[[nodiscard]] QStringList stringList(const QJsonValue &value) {
    QStringList list;
    if (value.isString())
        return BookMeta::splitAuthors(value.toString());
    for (const QJsonValue &item : value.toArray())
        if (const QString name = item.toString().simplified(); !name.isEmpty())
            list << name;
    return list;
}

} // namespace

// "Ann Lee & Bo Ek" and "Ann Lee; Bo Ek". A comma is not a separator, since
// "Lee, Ann" is one author.
QStringList BookMeta::splitAuthors(const QString &text) {
    static const QRegularExpression separator("\\s*(?:&|;|\\band\\b)\\s*");
    return splitList(text, separator);
}

QStringList BookMeta::splitTags(const QString &text) {
    static const QRegularExpression separator("\\s*[,;]\\s*");
    return splitList(text, separator);
}

void BookMeta::apply(const BookMeta &other) {
    if (!other.title.isEmpty()) title = other.title;
    if (!other.authors.isEmpty()) authors = other.authors;
    if (!other.series.isEmpty()) {
        series = other.series;
        seriesIndex = other.seriesIndex;
    }
    if (!other.publisher.isEmpty()) publisher = other.publisher;
    if (!other.language.isEmpty()) language = other.language;
    if (!other.isbn.isEmpty()) isbn = other.isbn;
    if (!other.published.isEmpty()) published = other.published;
    if (!other.description.isEmpty()) description = other.description;
    if (!other.tags.isEmpty()) tags = other.tags;
    if (!other.cover.isNull()) cover = other.cover;
}

void BookMeta::fillFrom(const BookMeta &other) {
    BookMeta merged = other;
    merged.apply(*this);
    *this = std::move(merged);
}

QJsonObject BookMeta::toJson() const {
    return {
        {"title", title},
        {"authors", QJsonArray::fromStringList(authors)},
        {"series", series},
        {"series_index", seriesIndex},
        {"publisher", publisher},
        {"language", language},
        {"isbn", isbn},
        {"published", published},
        {"description", description},
        {"tags", QJsonArray::fromStringList(tags)},
    };
}

BookMeta BookMeta::fromJson(const QJsonObject &object) {
    BookMeta meta;
    meta.title = object.value("title").toString().simplified();
    meta.authors = stringList(object.value("authors"));
    meta.series = object.value("series").toString().simplified();
    meta.seriesIndex = object.value("series_index").toDouble();
    meta.publisher = object.value("publisher").toString().simplified();
    meta.language = object.value("language").toString().simplified();
    meta.isbn = object.value("isbn").toString().simplified();
    meta.published = object.value("published").toString().simplified();
    meta.description = object.value("description").toString().trimmed();
    meta.tags = object.value("tags").isString() ? splitTags(object.value("tags").toString())
                                                : stringList(object.value("tags"));
    return meta;
}

void BookMeta::applyJson(const QJsonObject &object) {
    const BookMeta given = fromJson(object);
    const auto take = [&object](const char *key, auto &field, const auto &value) {
        if (object.contains(QLatin1StringView(key)))
            field = value;
    };
    take("title", title, given.title);
    take("authors", authors, given.authors);
    take("series", series, given.series);
    take("series_index", seriesIndex, given.seriesIndex);
    take("publisher", publisher, given.publisher);
    take("language", language, given.language);
    take("isbn", isbn, given.isbn);
    take("published", published, given.published);
    take("description", description, given.description);
    take("tags", tags, given.tags);
}
