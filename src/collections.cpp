#include "collections.h"

#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <cstdio>
#include <optional>

#include "bookops.h"
#include "curation.h"
#include "readingformat.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

void print(FILE *stream, const QString &text) {
    std::fputs(text.toUtf8().constData(), stream);
    std::fputc('\n', stream);
}

int fail(const QString &why) {
    print(stderr, why);
    return 1;
}

void printJson(const QJsonValue &value) {
    print(stdout, QString::fromUtf8(value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented)
                                                    : QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented))
                      .trimmed());
}

constexpr auto usage = "Usage: omabook collections [list|add|rule|rename|move|include|exclude|follow|remove|show|format|sort] …\n\n"
                       "Collections are named parts of the library, named as you like: a book can be in several, and\n"
                       "nothing is copied or moved. Each has a rule, a search as omabook list takes it, and the books put\n"
                       "in or taken out by hand, which win over the rule. Collections nest: a path such as Studie/Python\n"
                       "names one inside another, and a collection holds what the ones inside it hold.\n\n"
                       "  list                              The collections as a tree, with how many books each holds\n"
                       "  add <path> [--rule <search>] [--parent <path>]  Make a collection (and the ones its path goes through)\n"
                       "  rule <path> <search>              Change what fills it (\"\" for only books put in by hand)\n"
                       "  rename <path> <new name|new path> Call it something else, or move it with a path; what is inside goes along\n"
                       "  move <path> <parent path|\"\">     Put it inside another collection, or at the top with \"\"\n"
                       "  include <name> <id>...            Put books in it, whatever the rule says\n"
                       "  exclude <name> <id>...            Take books out of it, whatever the rule says\n"
                       "  follow <name> <id>...             Let the rule decide for these books again\n"
                       "  remove <name>                     Take the collection away; its books stay in the library\n"
                       "  show <name>                       The books in it\n"
                       "  format <name> [--set key=value]   Its own reading format, between the defaults and a book's own\n"
                       "  sort [<name>] [--apply|--pick 1,3=New/Path]  Have the AI propose where the books not yet placed\n"
                       "                                    belong, in collections there are or new ones; what it can't place\n"
                       "                                    is listed to decide. With a collection's name, it proposes\n"
                       "                                    collections inside that one for the books in it that are in none\n"
                       "                                    of its own yet; they stay in it too. Picking takes from the list\n"
                       "                                    the last sort gave, asking nothing again (--again asks afresh);\n"
                       "                                    --pick N=path takes a proposal under another name\n\n"
                       "The search language: bare words, author:, title:, series:, tag:, genre: (fiction, nonfiction, or a\n"
                       "tag), format:, lang:, publisher:, about:, status:, is: and rating:; \"quotes\" keep words together,\n"
                       "-word leaves out, and OR joins alternatives: genre:fiction OR series:Dune -tag:poetry\n\n"
                       "Every subcommand takes --json and --library <folder>. omabook list --collection <name> lists one.";

QString brief(const Book &book) {
    return QString("%1\t%2\t%3\t%4").arg(book.id).arg(book.meta.title, book.meta.authorText(), book.formats.join(','));
}

// The books named on the command line by their numbers, or what is wrong.
std::expected<QList<qint64>, QString> bookIds(const Library &library, const QStringList &words) {
    QList<qint64> ids;
    for (const QString &word : words) {
        bool number = false;
        const qint64 id = word.toLongLong(&number);
        if (!number || !library.book(id))
            return std::unexpected(QString("There is no book %1 in the library; name books by their numbers from omabook list.").arg(word));
        ids << id;
    }
    if (ids.isEmpty())
        return std::unexpected(QString("Name the books by their numbers from omabook list."));
    return ids;
}

// Where the last sort's proposals are kept for picking from: this
// computer's state folder, one file per library.
QString keptFile(const Library &library) {
    const QString state = qEnvironmentVariableIsSet("XDG_STATE_HOME") ? qEnvironmentVariable("XDG_STATE_HOME")
                                                                      : QDir::homePath() + "/.local/state";
    return state + "/omabook/collections-sort-"
           + QString::fromLatin1(QCryptographicHash::hash(library.root().toUtf8(), QCryptographicHash::Sha1).toHex().left(16)) + ".json";
}

void keepProposals(const Library &library, const QList<Proposal> &proposals) {
    QJsonArray array;
    for (const Proposal &proposal : proposals)
        array << QJsonObject{{"kind", proposal.kind}, {"id", proposal.bookId}, {"collection_id", proposal.collectionId},
                             {"collection", proposal.collection}, {"why", proposal.why}};
    QDir().mkpath(QFileInfo(keptFile(library)).absolutePath());
    QSaveFile file(keptFile(library));
    if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(array).toJson(QJsonDocument::Compact)) > 0)
        file.commit();
}

// The proposals the last sort listed, of books still here; none when there was no sort.
std::optional<QList<Proposal>> keptProposals(const Library &library) {
    QFile file(keptFile(library));
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    QList<Proposal> proposals;
    for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).array()) {
        const QJsonObject object = value.toObject();
        const auto book = library.book(object.value("id").toInteger());
        if (!book)
            continue;
        Proposal proposal;
        proposal.kind = object.value("kind").toString();
        proposal.bookId = book->id;
        proposal.meta = book->meta;
        proposal.collectionId = object.value("collection_id").toInteger();
        proposal.collection = object.value("collection").toString();
        proposal.why = object.value("why").toString();
        proposals << proposal;
    }
    return proposals;
}

} // namespace

QJsonArray Collections::list(const Library &library) {
    const QList<Book> books = library.books();
    QJsonArray array;
    for (const Collection &collection : library.collections()) {
        QJsonObject object = collection.toJson();
        object.insert("count", int(collection.of(books).size()));
        object.insert("depth", collection.depth);
        array << object;
    }
    return array;
}

QJsonObject Collections::show(const Library &library, const Collection &collection) {
    QJsonObject object = collection.toJson();
    QJsonArray books;
    for (const Book &book : collection.of(library.books())) {
        QJsonObject described = book.toJson();
        described.insert("by_hand", collection.included.contains(book.id));
        books << described;
    }
    object.insert("count", books.size());
    object.insert("books", books);
    return object;
}

int Collections::run(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription("Collections: named parts of the library.");
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"library", "Use the library in this folder instead of the current one.", "folder"});
    parser.addOption({"rule", "add: the search that fills it, as omabook list takes it.", "search"});
    parser.addOption({"set", "format: an option and its value, such as align=justify or measure=72; may be given more than once.",
                      "key=value"});
    parser.addOption({"reset", "format: the collection follows the defaults again."});
    parser.addOption({"apply", "sort: put every book in the collection proposed for it."});
    parser.addOption({"pick", "sort: only these proposals, by their numbers in the list: 1,3,4; 3=Some/Path puts that one in "
                              "a collection of that path instead.", "numbers"});
    parser.addOption({"parent", "add: make it inside this collection.", "path"});
    parser.addOption({"again", "sort: ask the AI again rather than pick from the list the last sort gave."});
    parser.addOption({{"h", "help"}, "Show this help."});
    QStringList rest = arguments;
    rest.removeAt(1);
    parser.process(rest);
    const QStringList words = parser.positionalArguments();
    const QString sub = words.value(0, u"list"_s);
    const bool json = parser.isSet("json");
    if (parser.isSet("help")) {
        print(stdout, usage);
        return 0;
    }
    Library library(parser.isSet("library") ? parser.value("library") : Settings().libraryPath());
    QString error;
    if (!library.open(&error))
        return fail(error);

    if (sub == "list") {
        const QJsonArray collections = list(library);
        if (json) {
            printJson(collections);
        } else if (collections.isEmpty()) {
            print(stdout, "There are no collections yet. Make one with: omabook collections add <name> --rule <search>, "
                          "or have the AI propose some with: omabook collections sort");
        } else {
            // The tree: each inside the one above it, indented.
            for (const QJsonValue &value : collections) {
                const QJsonObject collection = value.toObject();
                print(stdout, QString("%1\t%2%3\t%4 books\t%5").arg(collection.value("id").toInteger())
                                  .arg(QString(collection.value("depth").toInt() * 2, ' '), collection.value("name").toString())
                                  .arg(collection.value("count").toInt()).arg(collection.value("rule").toString()));
            }
        }
        return 0;
    }
    if (sub == "add") {
        if (words.size() < 2)
            return fail(usage);
        qint64 parent = 0;
        if (parser.isSet("parent") && !parser.value("parent").trimmed().isEmpty()) {
            const auto inside = library.collectionNamed(parser.value("parent"));
            if (!inside)
                return fail("There is no collection called " + parser.value("parent") + " to put it in.");
            parent = inside->id;
        }
        const auto made = library.addCollection(words.mid(1).join(' '), parser.value("rule"), parent);
        if (!made)
            return fail(made.error());
        const auto collection = library.collection(*made);
        if (json)
            printJson(show(library, *collection));
        else
            print(stdout, QString("%1\t%2\t%3 books").arg(*made).arg(collection->path)
                              .arg(collection->of(library.books()).size()));
        return 0;
    }
    if (sub == "sort") {
        QList<Collection> collections = library.collections();
        QList<Book> books = library.books();
        // Within one collection: its books not yet in one of its own, sorted
        // into the collections inside it, there or new.
        const QString within = words.mid(1).join(' ').trimmed();
        std::optional<Collection> scope;
        if (!within.isEmpty()) {
            scope = library.collectionNamed(within);
            if (!scope)
                return fail("There is no collection " + within + ". omabook collections lists them.");
            const QString prefix = scope->path + "/";
            books = scope->of(books);
            collections.removeIf([&](const Collection &collection) { return !collection.path.startsWith(prefix); });
        }
        // Picking from the list asks nothing again: the proposals are the
        // ones the last sort of this library listed, numbers and all. Only
        // a sort without --pick or --apply (or with --again) asks the AI.
        const bool choosing = parser.isSet("pick") || parser.isSet("apply");
        QList<Proposal> proposals;
        bool saved = false;
        if (choosing && !parser.isSet("again"))
            if (const auto kept = keptProposals(library)) {
                proposals = *kept;
                saved = true;
            }
        // A request per batch, each bounded: the books not yet placed. The
        // new collections a batch proposes go along to the next.
        for (const QList<Book> &batch : saved ? QList<QList<Book>>() : Curation::uncollected(books, collections)) {
            QStringList proposed;
            for (const Proposal &proposal : std::as_const(proposals))
                if (proposal.kind == "collection" && proposal.collectionId == 0 && !proposed.contains(proposal.collection))
                    proposed << proposal.collection;
            QString answer;
            AiRequest request = Curation::collectionsRequest(batch, collections, books, proposed);
            if (scope)
                request.system += "\n\nEvery book given is already in the collection \"" + scope->path
                                  + "\", and stays there. Place each in a collection inside it: every path you give "
                                    "starts with \"" + scope->path + "/\". When none inside it fits and a new one "
                                    "wouldn't have company, leave the book undecided.";
            if (!BookOps::ask(request, &answer, &error))
                return fail(error);
            for (Proposal proposal : Curation::collectionsFrom(answer, batch, collections)) {
                // A place outside the collection sorted is no answer here.
                if (scope && proposal.kind != "undecided" && !proposal.collection.startsWith(scope->path + "/")) {
                    proposal.kind = "undecided";
                    proposal.why = "proposed outside " + scope->path + ": " + proposal.collection;
                }
                proposals << proposal;
            }
        }
        if (!saved)
            keepProposals(library, proposals);
        // "1,3=Some/Path": the numbers picked, and another path for some.
        QHash<int, QString> picked;
        for (const QString &item : parser.value("pick").split(',', Qt::SkipEmptyParts))
            picked.insert(item.section('=', 0, 0).trimmed().toInt(), item.section('=', 1).trimmed());
        QJsonArray array, undecided;
        int applied = 0, number = 0;
        QStringList problems;
        for (Proposal proposal : std::as_const(proposals)) {
            if (proposal.kind == "undecided") {
                undecided << QJsonObject{{"id", proposal.bookId}, {"title", proposal.meta.title},
                                         {"authors", proposal.meta.authorText()}, {"why", proposal.why}};
                continue;
            }
            ++number;
            const bool fresh = proposal.collectionId == 0;
            array << QJsonObject{{"number", number}, {"id", proposal.bookId}, {"title", proposal.meta.title},
                                 {"authors", proposal.meta.authorText()}, {"collection", proposal.collection},
                                 {"new", fresh}, {"why", proposal.why}};
            if (!json)
                print(stdout, QString("%1\t%2\t%3\t-> %4%5\t%6").arg(number).arg(proposal.bookId)
                                  .arg(proposal.meta.title, proposal.collection, fresh ? " (new)" : "", proposal.why));
            if (parser.isSet("pick") ? !picked.contains(number) : !parser.isSet("apply"))
                continue;
            if (!picked.value(number).isEmpty()) {
                proposal.collection = picked.value(number);
                proposal.collectionId = 0;
            }
            if (Curation::apply(library, proposal, &error))
                ++applied;
            else
                problems << error;
        }
        if (!json && !undecided.isEmpty()) {
            print(stdout, "\nTo decide yourself (omabook collections include <path> <id>):");
            for (const QJsonValue &value : std::as_const(undecided)) {
                const QJsonObject book = value.toObject();
                print(stdout, QString("  %1\t%2\t%3").arg(book.value("id").toInteger()).arg(book.value("title").toString(),
                                                                                           book.value("why").toString()));
            }
        }
        for (const QString &problem : std::as_const(problems))
            print(stderr, problem);
        // What was applied is done with; another sort asks afresh.
        if (choosing && applied > 0)
            QFile::remove(keptFile(library));
        if (json)
            printJson(QJsonObject{{"proposals", array}, {"to_decide", undecided}, {"applied", applied}});
        else if (parser.isSet("apply") || parser.isSet("pick"))
            print(stdout, QString("%1 books put in collections").arg(applied));
        else if (proposals.isEmpty())
            print(stdout, scope ? "Every book in " + scope->path + " is in a collection inside it, or the AI had nothing to propose."
                                : QString("Every book is placed, or the AI had nothing to propose."));
        return 0;
    }
    if (sub == "take") {
        // What a sync hands over from another computer: a file of collections.
        QFile file(words.value(1));
        if (!file.open(QIODevice::ReadOnly))
            return fail("There is no file " + words.value(1) + " to take collections from.");
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        const int changed = take(library, document.isArray() ? document.array() : document.object().value("collections").toArray());
        if (json)
            printJson(QJsonObject{{"changed", changed}});
        else
            print(stdout, QString("%1 collections brought up to date").arg(changed));
        return 0;
    }

    // The rest name a collection.
    if (words.size() < 2)
        return fail(usage);
    const auto collection = library.collectionNamed(words.value(1));
    if (!collection)
        return fail("There is no collection called " + words.value(1) + ". omabook collections lists them.");
    const qint64 id = collection->id;
    bool done = true;
    if (sub == "show") {
        const QJsonObject shown = show(library, *collection);
        if (json) {
            printJson(shown);
        } else {
            for (const Book &book : collection->of(library.books()))
                print(stdout, brief(book));
        }
        return 0;
    }
    if (sub == "rule") {
        done = library.setCollectionRule(id, words.mid(2).join(' '), &error);
    } else if (sub == "rename") {
        // A path moves it as well: what is inside it goes along.
        QStringList parts = words.mid(2).join(' ').split('/', Qt::SkipEmptyParts);
        for (QString &part : parts)
            part = part.simplified();
        parts.removeAll(QString());
        if (parts.isEmpty())
            return fail("Give the new name, or a path to move it to.");
        const QString leaf = parts.takeLast();
        qint64 parent = collection->parent;
        if (words.mid(2).join(' ').contains('/')) {
            parent = 0;
            if (!parts.isEmpty()) {
                const auto inside = library.collectionNamed(parts.join('/'));
                const auto made = inside ? std::expected<qint64, QString>(inside->id) : library.addCollection(parts.join('/'));
                if (!made)
                    return fail(made.error());
                parent = *made;
            }
        }
        done = library.setCollectionParent(id, parent, &error) && library.renameCollection(id, leaf, &error);
    } else if (sub == "move") {
        qint64 parent = 0;
        if (!words.value(2).trimmed().isEmpty()) {
            const auto inside = library.collectionNamed(words.mid(2).join(' '));
            if (!inside)
                return fail("There is no collection called " + words.mid(2).join(' ') + " to put it in.");
            parent = inside->id;
        }
        done = library.setCollectionParent(id, parent, &error);
    } else if (sub == "include" || sub == "exclude" || sub == "follow") {
        const auto ids = bookIds(library, words.mid(2));
        if (!ids)
            return fail(ids.error());
        const Library::Batch batch(library);
        for (const qint64 book : *ids)
            done = (sub == "follow" ? library.followRule(id, book) : library.putInCollection(id, book, sub == "include")) && done;
    } else if (sub == "remove") {
        done = library.removeCollection(id);
        if (done) {
            if (json)
                printJson(QJsonObject{{"removed", collection->name}});
            else
                print(stdout, "Removed the collection " + collection->name + "; its books stay in the library.");
            return 0;
        }
    } else if (sub == "format") {
        QJsonObject options = parser.isSet("reset") ? QJsonObject() : collection->format;
        for (const QString &pair : parser.values("set")) {
            const QString key = pair.section('=', 0, 0).trimmed(), text = pair.section('=', 1).trimmed();
            bool number = false;
            const int value = text.toInt(&number);
            const QJsonValue given = number ? QJsonValue(value) : QJsonValue(text);
            if (const QString why = ReadingFormat::problem(key, given); !why.isEmpty())
                return fail(why);
            options.insert(key, given);
        }
        done = library.setCollectionFormat(id, options);
        if (done) {
            const QJsonObject own = library.collection(id)->format;
            if (json) {
                printJson(QJsonObject{{"collection", collection->name}, {"format", own}});
            } else if (own.isEmpty()) {
                print(stdout, collection->name + " follows the defaults.");
            } else {
                for (auto it = own.begin(); it != own.end(); ++it)
                    print(stdout, it.key() + "\t" + it.value().toVariant().toString());
            }
            return 0;
        }
        error = "The library can't be written.";
    } else {
        return fail(usage);
    }
    if (!done)
        return fail(error.isEmpty() ? u"The library can't be written."_s : error);
    const auto now = library.collection(id);
    if (json)
        printJson(now ? QJsonObject(show(library, *now)) : QJsonObject());
    else if (now)
        print(stdout, QString("%1\t%2\t%3 books\t%4").arg(now->id).arg(now->path).arg(now->of(library.books()).size()).arg(now->rule));
    return 0;
}
