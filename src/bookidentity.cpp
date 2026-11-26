#include "bookidentity.h"

#include <QRegularExpression>
#include <QUuid>
#include <ranges>

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString folded(const QString &text) {
    static const QRegularExpression other(R"([^\p{L}\p{N}]+)");
    return text.toCaseFolded().remove(other);
}

// A title without its subtitle: what comes before a colon or a dash between
// words, and without what is in brackets at its end ("(Series, #1)").
[[nodiscard]] QString mainTitle(const QString &title) {
    static const QRegularExpression subtitle(R"(\s*(:|\s[-\x{2013}\x{2014}]\s).*$)");
    static const QRegularExpression bracketed(R"(\s*[(\[][^)\]]*[)\]]\s*$)");
    QString main = title;
    main.remove(bracketed);
    main.remove(subtitle);
    return main.trimmed().isEmpty() ? title : main;
}

[[nodiscard]] std::optional<QString> goodreads(const QString &value) {
    static const QRegularExpression number(R"(^\s*(\d+))");
    const QRegularExpressionMatch match = number.match(value);
    if (!match.hasMatch())
        return std::nullopt;
    QString digits = match.captured(1);
    while (digits.size() > 1 && digits.startsWith(u'0'))
        digits.remove(0, 1);
    return digits == "0"_L1 ? std::nullopt : std::optional(digits);
}

[[nodiscard]] std::optional<QString> calibreUuid(const QString &value) {
    const QUuid uuid = QUuid::fromString(value.trimmed());
    return uuid.isNull() ? std::nullopt : std::optional(uuid.toString(QUuid::WithoutBraces));
}

[[nodiscard]] int digit(QChar c) { return c.digitValue(); }

[[nodiscard]] std::optional<QString> fromIsbn10(const QString &number) {
    int sum = 0;
    for (const auto [at, c] : std::views::enumerate(number)) {
        const int value = (at == 9 && (c == u'X' || c == u'x')) ? 10 : digit(c);
        if (value < 0 || (value == 10 && at != 9))
            return std::nullopt;
        sum += static_cast<int>(10 - at) * value;
    }
    if (sum % 11 != 0)
        return std::nullopt;
    const QString stem = u"978"_s + number.left(9);
    int check = 0;
    for (const auto [at, c] : std::views::enumerate(stem))
        check += digit(c) * (at % 2 == 0 ? 1 : 3);
    return stem + QString::number((10 - check % 10) % 10);
}

[[nodiscard]] std::optional<QString> validIsbn13(const QString &number) {
    if (!number.startsWith("978"_L1) && !number.startsWith("979"_L1))
        return std::nullopt;
    int sum = 0;
    for (const auto [at, c] : std::views::enumerate(number)) {
        if (digit(c) < 0)
            return std::nullopt;
        sum += digit(c) * (at % 2 == 0 ? 1 : 3);
    }
    return sum % 10 == 0 ? std::optional(number) : std::nullopt;
}

} // namespace

std::optional<QString> BookIdentity::isbn13(const QString &text) {
    static const QRegularExpression spacing(R"([\s-]+)");
    QString number = text;
    number.remove(spacing);
    if (number.size() == 10)
        return fromIsbn10(number);
    if (number.size() == 13)
        return validIsbn13(number);
    return std::nullopt;
}

QStringList BookIdentity::strongKeys(const QJsonObject &identifiers, const QString &isbn) {
    const auto said = [&identifiers](const char *scheme) {
        for (auto it = identifiers.begin(); it != identifiers.end(); ++it)
            if (it.key().compare(QLatin1StringView(scheme), Qt::CaseInsensitive) == 0)
                return it.value().toVariant().toString();
        return QString();
    };
    QStringList keys;
    for (const QString &number : {isbn, said("isbn")})
        if (const std::optional<QString> valid = isbn13(number))
            keys << u"entry:isbn="_s + *valid;
    if (const std::optional<QString> id = goodreads(said("goodreads")))
        keys << u"entry:goodreads="_s + *id;
    if (const std::optional<QString> uuid = calibreUuid(said("calibre")))
        keys << u"entry:calibre="_s + *uuid;
    keys.removeDuplicates();
    return keys;
}

QStringList BookIdentity::strongKeys(const Book &book) { return strongKeys(book.identifiers, book.meta.isbn); }

QStringList BookIdentity::idKeys(const Book &book) {
    QStringList keys = strongKeys(book);
    for (const QString &key : Library::entryKeys(book))
        if (key.startsWith("entry:title="_L1))
            keys << key;
    return keys;
}

bool BookIdentity::titlesAgree(const QString &one, const QString &other) {
    const QString first = folded(one);
    const QString second = folded(other);
    if (first.isEmpty() || second.isEmpty())
        return first == second;
    return first == second || folded(mainTitle(one)) == second || first == folded(mainTitle(other));
}

bool BookIdentity::strongId(const QString &gid) {
    if (gid.startsWith("b:"_L1) || gid.startsWith("u:"_L1) || gid.startsWith("e:entry:title="_L1))
        return true;
    const auto value = [&gid](QLatin1StringView prefix) {
        return gid.startsWith(prefix) ? std::optional(gid.mid(prefix.size())) : std::nullopt;
    };
    if (const auto isbn = value("e:entry:isbn="_L1))
        return isbn13(*isbn).has_value();
    if (const auto id = value("e:entry:goodreads="_L1))
        return goodreads(*id).has_value();
    if (const auto uuid = value("e:entry:calibre="_L1))
        return calibreUuid(*uuid).has_value();
    return false;
}

// What names a book without a file across libraries (declared in library.h).
QStringList Library::entryKeys(const QJsonObject &identifiers, const QString &isbn, const QString &title, const QStringList &authors) {
    QStringList keys; // bookkeeping ("goodreads:synced", the same of every book to read; "generated") names no book (#154)
    for (auto it = identifiers.begin(); it != identifiers.end(); ++it)
        if (const QString value = it.value().toVariant().toString().trimmed();
            !value.isEmpty() && !it.key().contains(u':') && it.key().compare("generated"_L1, Qt::CaseInsensitive) != 0)
            keys << "entry:" + it.key().toLower() + "=" + value.toLower();
    if (const QString number = folded(isbn); !number.isEmpty())
        keys << "entry:isbn=" + number;
    if (const QString name = folded(title); !name.isEmpty()) {
        QStringList named;
        for (const QString &author : authors)
            named << folded(author);
        named.sort();
        keys << "entry:title=" + name + "|" + named.join(',');
    }
    keys.removeDuplicates();
    return keys;
}

QStringList Library::entryKeys(const Book &book) { return entryKeys(book.identifiers, book.meta.isbn, book.meta.title, book.meta.authors); }
