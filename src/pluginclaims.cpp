#include "pluginclaims.h"

#include <algorithm>

#include "formats.h"

QString PluginClaims::owner(const QStringList &formats, const Registered &plugins) {
    if (formats.isEmpty() || std::ranges::any_of(formats, Formats::ownFormat))
        return {};
    QString found;
    for (const auto &[plugin, extensions] : plugins) {
        if (!std::ranges::all_of(formats, [&extensions](const QString &format) { return extensions.contains(format); }))
            continue;
        // Two plugins that both read it: neither may call it its own.
        if (!found.isEmpty())
            return {};
        found = plugin;
    }
    return found;
}

QList<qint64> PluginClaims::claim(Library &library, const QList<Book> &books, const Registered &plugins) {
    QList<qint64> claimed;
    for (const Book &book : books) {
        if (!book.identifiers.value("generated").toString().isEmpty())
            continue;
        if (const QString plugin = owner(book.formats, plugins); !plugin.isEmpty() && library.setIdentifier(book.id, "generated", plugin))
            claimed << book.id;
    }
    return claimed;
}
