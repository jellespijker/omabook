#include "spacecommands.h"

#include <QCommandLineParser>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <print>
#include <tuple>

#include "failure.h"
#include "library.h"
#include "librarymigrations.h"
#include "person.h"
#include "scriptplugins.h"
#include "scriptsharing.h"
#include "settings.h"
#include "sharedspaces.h"
#include "spacefiles.h"
#include "spaceparts.h"
#include "spaceswitch.h"

namespace {

using namespace Qt::StringLiterals;
using SharedSpaces::Summary;

constexpr auto usage = "Usage: omabook spaces [list|create|invite|remove|join|sync|files|annotations|fetch] …\n\n"
                       "Every piece of your library belongs to a space: your personal one (your own computers), a collection\n"
                       "you published or subscribed to, or a shared space several people write, as a household library or a\n"
                       "book club. A shared space holds a collection (its tree and its books' details; their files only when\n"
                       "a writer switches them on), and each member links a collection of their own to it. Writers change it\n"
                       "by changing that collection; readers get every change and write nothing. Who is a member, and as\n"
                       "what, travels in the space itself, signed: an operation by anyone who wasn't a writer at its time\n"
                       "is refused.\n\n"
                       "  list                                      The spaces you are in, their kind, your role and the members\n"
                       "  create <name> [--collection c] [--parts p]  Found a shared space of a collection (the name's, or c);\n"
                       "                                            p: graph,questions,ratings,reading,annotations\n"
                       "  invite <space> <person-id> [--role r] [--out f]  Make someone a member (reader or writer; writer\n"
                       "                                            unless said) and write their invite: a file only they can open\n"
                       "  remove <space> <person-id>                What they write from now on is refused; what they had stays theirs\n"
                       "  join <invite-file> [--as name]            Join the space an invite is for; its books go in a collection\n"
                       "  sync <space> --folder <dir>               Take what is new in a folder something else syncs (Syncthing,\n"
                       "                                            a mounted share), and write what is new here into it, sealed\n"
                       "  files <space> on|off                      Let the books' files of a shared space (as a writer) or of one\n"
                       "                                            you published travel to everyone it reaches; off unless switched\n"
                       "  annotations <space> on|off                Carry its members' highlights, margin notes and ink into it,\n"
                       "                                            each seen by the others as far as they have read; off unless\n"
                       "                                            switched\n"
                       "  fetch <book>                              Fetch a book's file from the space that carries it, checked\n"
                       "                                            against its hash; a book with a file of its own keeps it\n\n"
                       "A person's id is what omabook identity --export prints. Invite someone, then sync, so the operation that\n"
                       "makes them a member reaches the folder before they write. Every subcommand takes --json and\n"
                       "--library <folder>.";

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

int fail(const QString &why) {
    printLine(stderr, why);
    return 1;
}

void printJson(const QJsonValue &value) {
    const QJsonDocument document = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
    printLine(stdout, QString::fromUtf8(document.toJson(QJsonDocument::Indented)).trimmed());
}

[[nodiscard]] QJsonObject toJson(const Summary &space) {
    QJsonArray members;
    for (const SharedSpaces::Member &member : space.members)
        members << QJsonObject{{"person", member.person}, {"name", member.name}, {"role", member.role}};
    return {{"space", space.space}, {"kind", space.kind},   {"name", space.name},
            {"role", space.role},   {"members", members}, {"collection", space.collection},
            {"files", space.files}};
}

[[nodiscard]] QStringList describe(const Summary &space) {
    QStringList lines{
        u"%1  (%2; you are %3)%4"_s.arg(space.name, space.kind, space.role.isEmpty() ? u"not a member yet"_s : u"a "_s + space.role,
                                        space.collection.isEmpty() ? QString() : u"  in the collection "_s + space.collection)};
    if (space.files)
        lines << u"  its books' files travel in it, to everyone it reaches"_s;
    lines << u"  "_s + space.space;
    for (const SharedSpaces::Member &member : space.members)
        lines << u"    %1  %2%3"_s.arg(member.role, member.person, member.name.isEmpty() ? QString() : u"  ("_s + member.name + u')');
    return lines;
}

// A person's id is URL-safe base64 and may begin with "-", which the parser
// would read as options. It is the last argument of every command that takes
// one, so such a word goes at the end, after "--", where nothing is an option.
[[nodiscard]] QStringList withIdsAsArguments(const QStringList &arguments) {
    QStringList rest;
    QStringList ids;
    for (const QString &word : arguments)
        (word.startsWith(u'-') && !word.startsWith("--"_L1) && Person::publicKeyFrom(word) ? ids : rest) << word;
    return ids.isEmpty() ? rest : rest + QStringList{u"--"_s} + ids;
}

struct Command {
    QCommandLineParser parser;
    QStringList words;
    bool json = false;
};

[[nodiscard]] int list(const Library &library, const Command &command) {
    const QList<Summary> spaces = SharedSpaces::list(library);
    if (command.json) {
        QJsonArray all;
        for (const Summary &space : spaces)
            all << toJson(space);
        printJson(all);
        return 0;
    }
    QStringList lines;
    for (const Summary &space : spaces)
        lines << describe(space);
    printLine(stdout, lines.join(u'\n'));
    return 0;
}

[[nodiscard]] int create(Library &library, const Command &command) {
    if (command.words.size() != 2)
        return fail(u"Name the space: omabook spaces create <name>"_s);
    const QStringList parts = command.parser.value("parts").split(u',', Qt::SkipEmptyParts);
    for (const QString &part : parts)
        if (SpaceParts::named(part.trimmed()) == nullptr)
            return fail(Failure(Failure::Code::UnknownPart, {u"--parts"_s, SpaceParts::names().join(", "_L1), part}).text());
    // The plugins are asked what of theirs goes with it (#65).
    Settings settings;
    CommandScripts scripts(&settings, library.root());
    const auto created =
        SharedSpaces::create(library, command.words.at(1), parts, command.parser.value("collection"), worldsOf(scripts.plugins()));
    if (!created)
        return fail(created.error());
    if (command.json) {
        QJsonObject result = toJson(created->space);
        result.insert("new_collection", created->newCollection);
        result.insert("operations", created->operations);
        printJson(result);
        return 0;
    }
    printLine(stdout, (QStringList{u"Founded the shared space %1, of the collection %2%3, with its key kept for you alone."_s.arg(
                           created->space.name, created->space.collection, created->newCollection ? u" (new)"_s : QString())}
                       + describe(created->space)
                       + QStringList{u"Invite people with omabook spaces invite, and sync it with omabook spaces sync --folder."_s})
                          .join(u'\n'));
    return 0;
}

[[nodiscard]] int invite(Library &library, const Command &command) {
    if (command.words.size() != 3)
        return fail(u"Name the space and the person: omabook spaces invite <space> <person-id>"_s);
    const QString roleName = command.parser.isSet("role") ? command.parser.value("role") : u"writer"_s;
    const auto role = SharedSpace::roleFrom(roleName);
    if (!role || *role == SharedSpace::Role::None)
        return fail(u"--role is reader or writer."_s);
    const auto invited = SharedSpaces::invite(library, command.words.at(1), command.words.at(2), *role, command.parser.value("out"));
    if (!invited)
        return fail(invited.error());
    if (command.json) {
        printJson(QJsonObject{
            {"space", toJson(invited->space)}, {"person", invited->person}, {"role", invited->role}, {"invite", invited->file}});
        return 0;
    }
    printLine(stdout, u"%1 is a %2 of %3 now. Their invite, which only they can open: %4\nSend it to them, and sync the space so "
                      "that they are a member when they join."_s.arg(invited->person, invited->role, invited->space.name, invited->file));
    return 0;
}

[[nodiscard]] int remove(const Library &library, const Command &command) {
    if (command.words.size() != 3)
        return fail(u"Name the space and the person: omabook spaces remove <space> <person-id>"_s);
    const auto removed = SharedSpaces::remove(library, command.words.at(1), command.words.at(2));
    if (!removed)
        return fail(removed.error());
    if (command.json) {
        printJson(toJson(*removed));
        return 0;
    }
    printLine(
        stdout,
        (QStringList{u"%1 is no longer a member: what they write from now on is refused, whatever its date, and the "
                     "space's key has changed, so they can't open what is written after; sync the space to hand the "
                     "new key to the others. What they had of the space stays theirs; nothing takes it back."_s.arg(command.words.at(2))}
         + describe(*removed))
            .join(u'\n'));
    return 0;
}

[[nodiscard]] int join(Library &library, const Command &command) {
    if (command.words.size() != 2)
        return fail(u"Name the invite: omabook spaces join <invite-file>"_s);
    const auto joined = SharedSpaces::join(library, command.words.at(1), command.parser.value("as"));
    if (!joined)
        return fail(joined.error());
    if (command.json) {
        printJson(toJson(*joined));
        return 0;
    }
    printLine(stdout,
              (QStringList{u"Joined %1. Sync it through the folder its members share: omabook spaces sync \"%1\" --folder <dir>"_s.arg(
                   joined->name)}
               + describe(*joined))
                  .join(u'\n'));
    return 0;
}

[[nodiscard]] int sync(Library &library, const Command &command) {
    if (command.words.size() != 2 || !command.parser.isSet("folder"))
        return fail(u"Name the space and the folder: omabook spaces sync <space> --folder <dir>"_s);
    const auto synced = SharedSpaces::sync(library, command.words.at(1), command.parser.value("folder"));
    if (!synced)
        return fail(synced.error());
    if (command.json) {
        printJson(QJsonObject{{"space", toJson(synced->space)},
                              {"batches", synced->batches},
                              {"taken", synced->taken},
                              {"known", synced->known},
                              {"written", synced->written},
                              {"sent", synced->sent},
                              {"files", synced->files},
                              {"fetched", synced->fetched},
                              {"refused", QJsonArray::fromStringList(synced->refused)},
                              {"filed", Subscriptions::toJson(synced->filed)}});
        return 0;
    }
    QStringList lines{u"%1: %2 operations taken from %3 batches (%4 had already), %5 of your edits written into the space, %6 "
                      "operations written to the folder."_s.arg(synced->space.name)
                          .arg(synced->taken)
                          .arg(synced->batches)
                          .arg(synced->known)
                          .arg(synced->written)
                          .arg(synced->sent)};
    if (synced->files > 0)
        lines << u"%1 of the books' files written to the folder, sealed."_s.arg(synced->files);
    if (synced->fetched > 0)
        lines << u"%1 of the books' files that came with their worlds fetched from it, checked."_s.arg(synced->fetched);
    if (!synced->refused.isEmpty())
        lines << u"Refused, and not taken:"_s;
    for (const QString &why : synced->refused)
        lines << u"  "_s + why;
    lines << u"Its books are in the collection %1."_s.arg(synced->filed.collection);
    printLine(stdout, lines.join(u'\n'));
    return 0;
}

[[nodiscard]] int files(const Library &library, const Command &command) {
    const QString to = command.words.value(2);
    if (command.words.size() != 3 || (to != "on"_L1 && to != "off"_L1))
        return fail(u"Name the space, and on or off: omabook spaces files <space> on|off"_s);
    const auto switched = SpaceFiles::set(library, command.words.at(1), to == "on"_L1);
    if (!switched)
        return fail(switched.error());
    if (command.json) {
        printJson(QJsonObject{{"space", switched->space},
                              {"kind", switched->kind},
                              {"name", switched->name},
                              {"files", switched->on},
                              {"changed", switched->changed},
                              {"said", SpaceFiles::said(*switched)}});
        return 0;
    }
    printLine(stdout, (switched->changed ? QString() : u"It was so already. "_s) + SpaceFiles::said(*switched));
    return 0;
}

// A part of a space switched on or off (SpaceSwitch): its annotations.
[[nodiscard]] int part(const Library &library, const Command &command, const QString &name) {
    const QString to = command.words.value(2);
    if (command.words.size() != 3 || (to != "on"_L1 && to != "off"_L1))
        return fail(u"Name the space, and on or off: omabook spaces %1 <space> on|off"_s.arg(name));
    const auto switched = SpaceSwitch::set(library, command.words.at(1), name, to == "on"_L1);
    if (!switched)
        return fail(switched.error().text());
    const QString said = switched->on ? u"The %1 in the books of %2 (highlights, margin notes and ink) go into it now: whoever it "
                                        "reaches sees each person's as far as they have read."_s.arg(name, switched->name)
                                      : u"The %1 in the books of %2 no longer go into it."_s.arg(name, switched->name);
    if (command.json) {
        printJson(QJsonObject{{"space", switched->space},
                              {"kind", switched->kind},
                              {"name", switched->name},
                              {"part", switched->part},
                              {"on", switched->on},
                              {"changed", switched->changed},
                              {"said", said}});
        return 0;
    }
    printLine(stdout, (switched->changed ? QString() : u"It was so already. "_s) + said);
    return 0;
}

[[nodiscard]] int fetch(Library &library, const Command &command) {
    if (command.words.size() != 2)
        return fail(u"Name the book by its number: omabook spaces fetch <book>"_s);
    bool number = false;
    const qint64 book = command.words.at(1).toLongLong(&number);
    if (!number)
        return fail(u"Name the book by its number (omabook list --json gives it)."_s);
    const auto fetched = SpaceFiles::fetch(library, book);
    if (!fetched)
        return fail(fetched.error());
    if (command.json) {
        printJson(QJsonObject{{"book", book},
                              {"space", fetched->space},
                              {"format", fetched->format},
                              {"hash", fetched->hash},
                              {"size", fetched->size}});
        return 0;
    }
    printLine(stdout, u"Fetched its %1 file from %2, checked against its hash."_s.arg(fetched->format.toUpper(), fetched->name));
    return 0;
}

} // namespace

int runSpacesCli(const QStringList &arguments) {
    Command command;
    QCommandLineParser &parser = command.parser;
    parser.addOption({{"h", "help"}, "Show this help."});
    parser.addPositionalArgument("command", "list, create, invite, remove, join, sync, files, annotations or fetch.", "[command]");
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"library",
                      "Use the library in this folder instead of the current one, for this run only "
                      "(omabook libraries --use changes the one in use).",
                      "folder"});
    parser.addOption({"collection", "create: the collection here to share (the space's name when not given).", "collection"});
    parser.addOption({"parts", "create: more than the collection, by name: " + SpaceParts::names().join(", "_L1) + ".", "parts"});
    parser.addOption({"role", "invite: reader or writer (writer when not given).", "role"});
    parser.addOption({"out", "invite: the file to write the invite to, or a folder for it (this one when not given).", "file"});
    parser.addOption({"as", "join: the collection here for the space's books (the space's name when not given).", "name"});
    parser.addOption({"folder", "sync: the folder the members' computers share.", "dir"});
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook spaces"_s;
    parser.process(withIdsAsArguments(rest));
    command.words = parser.positionalArguments();
    command.json = parser.isSet("json");
    // The usage text is the help: Qt's own would add a second usage line above it.
    if (parser.isSet("help")) {
        printLine(stdout, QString::fromUtf8(usage));
        return 0;
    }
    const QString sub = command.words.value(0, u"list"_s);
    Library library(parser.isSet("library") ? parser.value("library") : Settings().libraryPath());
    library.onBringingUpToDate(LibraryMigrations::sayOnStandardError);
    QString error;
    if (!library.open(&error))
        return fail(error);
    if (sub == "list"_L1)
        return list(library, command);
    if (sub == "create"_L1)
        return create(library, command);
    if (sub == "invite"_L1)
        return invite(library, command);
    if (sub == "remove"_L1)
        return remove(library, command);
    if (sub == "join"_L1)
        return join(library, command);
    if (sub == "sync"_L1)
        return sync(library, command);
    if (sub == "files"_L1)
        return files(library, command);
    if (sub == "fetch"_L1)
        return fetch(library, command);
    if (sub == "annotations"_L1)
        return part(library, command, sub);
    return fail(QString::fromUtf8(usage));
}
