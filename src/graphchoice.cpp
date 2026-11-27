#include "graphchoice.h"

#include <QStringList>
#include <optional>

#include "numberoptions.h"
#include "tenancy.h"

namespace {

// The book a command names: the one it reads, or the one it was met in.
qint64 bookOf(const QCommandLineParser &parser, const QString &sub, qint64 first) {
    return QStringList{"show", "export", "log"}.contains(sub) ? first : parser.value("book").toLongLong();
}

// Whether a command reads or adds to a scope of its own (a world) and names
// no tenant, book or thing: it does so in the world whole (Graph::ofScope, #112).
bool inAWorld(const QCommandLineParser &parser, const QString &sub, qint64 first) {
    return !parser.value("scope").isEmpty() && parser.value("collection").isEmpty() && bookOf(parser, sub, first) <= 0
           && !QStringList{"node", "link", "unlink", "merge", "remove"}.contains(sub);
}

// The tenant a command reads or writes in (Tenancy::chosen): the one named,
// or that of the book given, or failing a book that of the thing, the
// relation or the first thing named, when it is in one tenant alone.
std::expected<Tenant, QString> tenantOf(const Library &library, const QCommandLineParser &parser, const QString &sub, qint64 first) {
    const QString named = parser.value("collection");
    const qint64 bookId = bookOf(parser, sub, first);
    QList<Tenant> ofThing;
    if (sub == "unlink")
        ofThing = Tenancy::ofEdge(library, first);
    else if (QStringList{"node", "link", "merge", "remove"}.contains(sub))
        ofThing = Tenancy::ofNode(library, first);
    if (named.isEmpty() && bookId <= 0 && ofThing.size() == 1 && ofThing.first() != Tenant::everyone()
        && Tenancy::tenants(library).contains(ofThing.first()))
        return ofThing.first();
    return Tenancy::chosen(library, named, bookId);
}

// Why a command that reads or adds by hand can't: no tenant was named where one must be.
std::optional<QString> refused(const std::expected<Tenant, QString> &tenant, const QString &sub) {
    static const QStringList inTenant{"show",  "find",   "node",   "add",    "link",       "unlink",
                                      "merge", "remove", "export", "scopes", "copy-scope", "log"};
    return !tenant && inTenant.contains(sub) ? std::optional(tenant.error()) : std::nullopt;
}

} // namespace

std::expected<GraphChoice::Chosen, QString> GraphChoice::of(const Library &library, const QCommandLineParser &parser, const QString &sub,
                                                            qint64 first) {
    if (inAWorld(parser, sub, first))
        return Chosen{Graph::ofScope(library, parser.value("scope")), false};
    const std::expected<Tenant, QString> tenant = tenantOf(library, parser, sub, first);
    // The scopes there are, when no tenant is named: those of every tenant, each read whole.
    if (sub == "scopes" && !tenant && parser.value("collection").isEmpty())
        return Chosen{Graph::ofLibrary(library), true};
    if (const auto why = refused(tenant, sub))
        return std::unexpected(*why);
    return Chosen{Graph::forTenant(library, tenant.value_or(Tenant::unfiled())), false};
}

namespace {

// How many of the words after a command are numbers.
qsizetype numbersNamed(const QStringList &words) {
    static const QStringList one{"show", "export", "log", "node", "unlink", "remove", "connect"};
    const QString sub = words.value(0);
    if (sub == "build")
        return words.size();
    if (sub == "link" || sub == "merge")
        return 2;
    return one.contains(sub) ? 1 : 0;
}

} // namespace

std::optional<QString> GraphChoice::notNumbers(const QCommandLineParser &parser, const QStringList &words) {
    const qsizetype named = numbersNamed(words);
    if (const auto why = Numbers::notNumbers(words.mid(1, named)))
        return why;
    return Numbers::problem(parser, {Numbers::section("upto"),
                                     Numbers::section(),
                                     Numbers::fraction(),
                                     {.name = "book", .least = 1},
                                     {.name = "depth"},
                                     {.name = "limit", .least = 1}});
}
