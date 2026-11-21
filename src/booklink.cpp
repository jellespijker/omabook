#include "booklink.h"

#include <QUrl>

#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto scheme = "omabook";

} // namespace

std::optional<BookLink::Target> BookLink::parse(const QString &link) {
    const QUrl url(link.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1StringView(scheme) || url.host() != "book"_L1)
        return std::nullopt;
    // "/<book>" or "/<book>/section/<n>", a slash at the end or not.
    const QStringList parts = url.path().split(u'/', Qt::SkipEmptyParts);
    if (parts.isEmpty() || (parts.size() != 1 && parts.size() != 3))
        return std::nullopt;
    Target target{.book = parts.first(), .section = -1};
    if (parts.size() == 3) {
        bool number = false;
        const int section = parts.at(2).toInt(&number);
        if (parts.at(1) != "section"_L1 || !number || section < 0)
            return std::nullopt;
        target.section = section;
    }
    return target;
}

QString BookLink::make(const QString &book, int section) {
    const QString start = u"%1://book/%2"_s.arg(QLatin1StringView(scheme), QString::fromLatin1(QUrl::toPercentEncoding(book, ":")));
    return section >= 0 ? start + u"/section/"_s + QString::number(section) : start;
}

qint64 BookLink::find(const Library &library, const QString &book) {
    if (const qint64 known = Spaces::localId(library, u"book"_s, book); known > 0)
        return known;
    bool number = false;
    const qint64 id = book.toLongLong(&number);
    return number && id > 0 && library.book(id) ? id : 0;
}
