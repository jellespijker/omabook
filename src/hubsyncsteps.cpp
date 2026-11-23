#include "hubsyncsteps.h"

#include <QCommandLineParser>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <optional>

#include "booksetting.h"
#include "collections.h"
#include "graphsync.h"
#include "library.h"
#include "log.h"
#include "sync.h"
#include "synclog.h"
#include "syncplan.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] Failure refused(const QStringList &arguments) {
    return Failure(Failure::Code::HubSyncRefused, {(u"omabook "_s + arguments.join(u' ')).left(120)});
}

// A JSON document the computer sent, by its name in the sync's folders.
[[nodiscard]] std::expected<QJsonDocument, Failure> sent(const QString &named, const HubSyncSteps::Resolve &resolve) {
    const auto file = resolve(named);
    if (!file)
        return std::unexpected(file.error());
    QFile read(*file);
    if (!read.open(QIODevice::ReadOnly))
        return std::unexpected(Failure(Failure::Code::HubSyncNoPlace, {named}));
    return QJsonDocument::fromJson(read.readAll());
}

[[nodiscard]] std::expected<QJsonValue, Failure> replicate(Library &library, const QString &named, const HubSyncSteps::Resolve &resolve,
                                                           bool dryRun) {
    const auto request = sent(named, resolve);
    if (!request)
        return std::unexpected(request.error());
    QJsonObject asked = request->object();
    // A dry run is one whatever the request says.
    if (dryRun)
        asked.insert("dry_run"_L1, true);
    return SyncLog::answer(library, asked);
}

[[nodiscard]] std::expected<QJsonValue, Failure> take(Library &library, const QStringList &arguments,
                                                      const HubSyncSteps::Resolve &resolve) {
    const auto given = sent(arguments.value(3), resolve);
    if (!given)
        return std::unexpected(given.error());
    if (arguments.first() == u"collections"_s)
        return QJsonObject{
            {"changed", Collections::take(library, given->isArray() ? given->array() : given->object().value("collections"_L1).toArray())}};
    if (!given->isObject())
        return std::unexpected(refused(arguments));
    return QJsonObject{{"changed", GraphSync::take(library, given->object())}};
}

// The details and cover `add` was given, from the sync's folders.
[[nodiscard]] std::expected<BookMeta, Failure> describedBy(const QCommandLineParser &parser, const HubSyncSteps::Resolve &resolve) {
    BookMeta meta;
    if (parser.isSet(u"meta"_s)) {
        const auto details = sent(parser.value(u"meta"_s), resolve);
        if (!details)
            return std::unexpected(details.error());
        meta = BookMeta::fromJson(details->object());
    }
    if (parser.isSet(u"cover"_s)) {
        const auto cover = resolve(parser.value(u"cover"_s));
        if (!cover)
            return std::unexpected(cover.error());
        QImageReader reader(*cover);
        reader.setAutoTransform(true);
        meta.cover = reader.read();
    }
    return meta;
}

// A book on a real shelf, or one wished for, without a file.
[[nodiscard]] std::expected<QJsonValue, Failure> addEntry(Library &library, const BookMeta &meta, bool wish) {
    const Library::Added added = library.addEntry(meta, !wish);
    if (!added.error.isEmpty())
        return std::unexpected(Failure::said(added.error));
    if (wish)
        library.setReading(added.id, u"to-read"_s, 0, {});
    const auto book = library.book(added.id);
    return book ? book->toJson() : QJsonObject();
}

// More formats of a book there is already.
[[nodiscard]] std::expected<QJsonValue, Failure> addFormats(Library &library, qint64 id, const QStringList &files) {
    QString error;
    for (const QString &file : files)
        if (!library.addFormat(id, file, &error))
            return std::unexpected(Failure::said(error));
    const auto book = library.book(id);
    return book ? book->toJson() : QJsonObject();
}

[[nodiscard]] std::expected<QJsonValue, Failure> add(Library &library, const QStringList &arguments, const HubSyncSteps::Resolve &resolve) {
    QCommandLineParser parser;
    parser.addOptions({{u"to"_s, {}, u"id"_s},
                       {u"meta"_s, {}, u"file"_s},
                       {u"cover"_s, {}, u"file"_s},
                       {u"paper"_s, {}},
                       {u"wish"_s, {}},
                       {u"json"_s, {}}});
    if (!parser.parse(QStringList{u"omabook add"_s} + arguments.mid(1)))
        return std::unexpected(refused(arguments));
    QStringList files;
    for (const QString &named : parser.positionalArguments()) {
        const auto file = resolve(named);
        if (!file)
            return std::unexpected(file.error());
        files << *file;
    }
    const auto meta = describedBy(parser, resolve);
    if (!meta)
        return std::unexpected(meta.error());
    const bool fileless = parser.isSet(u"paper"_s) || parser.isSet(u"wish"_s);
    if (fileless && files.isEmpty())
        return addEntry(library, *meta, parser.isSet(u"wish"_s));
    if (parser.isSet(u"to"_s) && !files.isEmpty())
        return addFormats(library, parser.value(u"to"_s).toLongLong(), files);
    if (files.size() != 1 || fileless)
        return std::unexpected(refused(arguments));
    const Library::Added added = library.add(files.first(), *meta);
    if (!added.error.isEmpty())
        return std::unexpected(Failure::said(added.error));
    return QJsonArray{QJsonObject{{"file", parser.positionalArguments().first()}, {"id", added.id}, {"duplicate", added.duplicate}}};
}

[[nodiscard]] std::expected<QJsonValue, Failure> set(Library &library, const QStringList &arguments, const HubSyncSteps::Resolve &resolve) {
    // The files it names are the sync's, by their names there.
    QStringList given = arguments.mid(1);
    for (qsizetype at = 0; at < given.size(); ++at) {
        const QString &word = given.at(at);
        if (word.startsWith(u"--cover="_s) || word.startsWith(u"--meta="_s))
            return std::unexpected(refused(arguments));
        if ((word == u"--cover"_s || word == u"--meta"_s) && at + 1 < given.size()) {
            const auto file = resolve(given.at(at + 1));
            if (!file)
                return std::unexpected(file.error());
            given[++at] = *file;
        }
    }
    QCommandLineParser parser;
    parser.addOption({u"json"_s, {}});
    BookSetting::addOptions(parser);
    if (!parser.parse(QStringList{u"omabook set"_s} + given) || parser.positionalArguments().size() != 1)
        return std::unexpected(refused(arguments));
    const auto book = BookSetting::apply(library, parser.positionalArguments().first().toLongLong(), parser,
                                         [](const QString &said) { qCWarning(lcHub).noquote() << "A sync's book:" << said; });
    if (!book)
        return std::unexpected(book.error());
    return book->toJson();
}

// Whether it is `collections take --json <file>` or `graph take --json <file>`.
[[nodiscard]] bool takesIn(const QStringList &arguments) {
    return arguments.size() == 4 && QStringList{u"collections"_s, u"graph"_s}.contains(arguments.at(0)) && arguments.at(1) == u"take"_s
           && arguments.at(2) == u"--json"_s;
}

} // namespace

bool HubSyncSteps::writes(const QStringList &arguments, const Resolve &resolve) {
    if (arguments.value(0) == u"manifest"_s)
        return false;
    if (arguments.size() == 3 && arguments.at(0) == u"sync"_s && arguments.at(1) == u"--replicate"_s) {
        const auto request = sent(arguments.at(2), resolve);
        return request && !request->object().value("dry_run"_L1).toBool();
    }
    return true;
}

std::expected<QJsonValue, Failure> HubSyncSteps::run(Library &library, const QStringList &arguments, const Resolve &resolve, bool dryRun) {
    const QString command = arguments.value(0);
    if (arguments == QStringList{u"manifest"_s, u"--log"_s})
        return libraryManifest(library, ManifestFor::LogPeer);
    // Each book as the sync guard compares it, before and after (#187).
    if (arguments == QStringList{u"manifest"_s, u"--state"_s})
        return SyncPlan::toJson(SyncPlan::states(library));
    if (arguments.size() == 3 && command == u"sync"_s && arguments.at(1) == u"--replicate"_s)
        return replicate(library, arguments.at(2), resolve, dryRun);
    const bool takes = takesIn(arguments);
    if (!takes && command != u"add"_s && command != u"set"_s)
        return std::unexpected(refused(arguments));
    if (dryRun)
        return std::unexpected(Failure(Failure::Code::HubSyncDryRun));
    if (takes)
        return take(library, arguments, resolve);
    return command == u"add"_s ? add(library, arguments, resolve) : set(library, arguments, resolve);
}
