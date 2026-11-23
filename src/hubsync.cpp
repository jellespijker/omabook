#include "hubsync.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <optional>
#include <utility>

#include "hubkeeper.h"
#include "hubrequests.h"
#include "hubsyncsteps.h"
#include "library.h"
#include "log.h"
#include "pluginfiles.h"
#include "scripttools.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

struct HubSync::Visit {
    QString computer;
    QString library; // its name, as the keeper knows it
    QString device;  // of the computer's library, as it said (#212)
    bool dryRun = false;
    QTemporaryDir scratch;
    int folders = 0;
    bool snapshotted = false; // its moment snapshot taken, before its first write
    bool planned = false;     // asked for the books' state the sync guard compares (#187), as before its first write
    // The library's books' files and their covers', once asked: files it may fetch.
    std::optional<QSet<QString>> books;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

    explicit Visit(const QString &in) : scratch(QDir(in).filePath(u"omabook-hub-sync-XXXXXX"_s)) {}

    // Where `named`, a name in its folders, is; refused outside them.
    [[nodiscard]] std::expected<QString, Failure> resolve(const QString &named) const {
        const QString clean = QDir::cleanPath(named);
        if (named.isEmpty() || clean == u"."_s || QDir::isAbsolutePath(clean) || clean == u".."_s || clean.startsWith(u"../"_s))
            return std::unexpected(Failure(Failure::Code::HubSyncNoPlace, {named.left(120)}));
        return scratch.filePath(clean);
    }

    [[nodiscard]] std::expected<QJsonObject, Failure> folder();
    [[nodiscard]] std::expected<QJsonObject, Failure> put(const QJsonObject &asked) const;
    [[nodiscard]] std::expected<QJsonObject, Failure> fetch(const QJsonObject &asked, HubKeeper &keeper);
    // `asking`, when it is a file the hub gives this sync; empty when not.
    [[nodiscard]] std::expected<QString, Failure> given(const QString &asking, HubKeeper &keeper);
    [[nodiscard]] std::expected<QJsonObject, Failure> omabook(const QJsonObject &asked, HubKeeper &keeper);
};

namespace {

[[nodiscard]] QString canonical(const QString &file) { return QFileInfo(file).canonicalFilePath(); }

[[nodiscard]] QSet<QString> filesOf(const Library &library) {
    QSet<QString> files;
    for (const Book &book : library.books()) {
        for (const FormatFile &file : library.formatFiles(book.id))
            files << canonical(file.path);
        if (const QString cover = library.coverPath(book.id); !cover.isEmpty())
            files << canonical(cover);
    }
    files.remove(QString());
    return files;
}

// Whether `file` (a canonical path) is a plugin's file the hub keeps for the library (#212), and the
// library of `device` is one of its own computers, to which alone they go.
[[nodiscard]] bool pluginFileOf(const Library &library, const QString &device, const QString &file) {
    const QString kept = canonical(HubSyncSteps::pluginsOf(library.root()));
    if (kept.isEmpty() || !file.startsWith(kept + u'/') || !Spaces::isOwnComputer(library, device))
        return false;
    const QString under = file.mid(kept.size() + 1); // <plugin>/data/<path>
    return ScriptTools::validName(under.section(u'/', 0, 0)) && under.section(u'/', 1, 1) == u"data"_s
           && PluginFiles::travels(under.section(u'/', 2));
}

} // namespace

HubSync::HubSync(QString scratch) : m_scratch(scratch.isEmpty() ? QDir::tempPath() : std::move(scratch)) {}

HubSync::~HubSync() = default;

void HubSync::addTo(HubRequests &requests) {
    requests.add(u"sync"_s, [this](const QJsonObject &asked, HubKeeper &keeper) { return answer(asked, keeper); });
}

void HubSync::forgetIdle() {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_visits, [now](const auto &kept) { return now - kept.second->last > idle; });
}

std::expected<QJsonObject, Failure> HubSync::answer(const QJsonObject &asked, HubKeeper &keeper) {
    const QString computer = asked.value("computer"_L1).toString();
    if (computer.isEmpty())
        return std::unexpected(Failure(Failure::Code::HubSyncNoComputer));
    if (asked.value("step"_L1).toString() == u"begin"_s)
        return begin(asked, keeper);
    const auto found = m_visits.find(asked.value("visit"_L1).toString());
    // Another computer's visit is no visit of this one's.
    if (found == m_visits.end() || found->second->computer != computer) {
        if (found != m_visits.end())
            qCWarning(lcHub).noquote() << computer << "asked for a step of" << found->second->computer + u"'s sync: refused."_s;
        return std::unexpected(Failure(Failure::Code::HubSyncNotBegun));
    }
    found->second->last = std::chrono::steady_clock::now();
    if (asked.value("step"_L1).toString() == u"end"_s) {
        qCInfo(lcHub).noquote() << computer + u"'s sync of "_s + found->second->library << "ended.";
        m_visits.erase(found);
        return QJsonObject();
    }
    return step(*found->second, asked, keeper);
}

std::expected<QJsonObject, Failure> HubSync::begin(const QJsonObject &asked, HubKeeper &keeper) {
    forgetIdle();
    const QString computer = asked.value("computer"_L1).toString();
    if (!asked.value("format"_L1).isDouble())
        return std::unexpected(Failure(Failure::Code::HubBadRequest));
    const int theirs = asked.value("format"_L1).toInt();
    auto visit = std::make_unique<Visit>(m_scratch);
    visit->computer = computer;
    visit->dryRun = asked.value("dry_run"_L1).toBool();
    visit->device = asked.value("device"_L1).toString();
    int format = 0;
    const auto checked = keeper.write(
        asked.value("library"_L1).toString(), HubKeeper::Moment::None, [&](Library &library) -> std::expected<QJsonObject, Failure> {
            visit->library = QFileInfo(library.root()).fileName();
            format = library.formatVersion();
            if (format == theirs)
                return QJsonObject();
            const QStringList words{visit->library, QString::number(format), QString::number(theirs)};
            qCWarning(lcHub).noquote() << computer << u"(a library of format "_s + QString::number(theirs) + u")"_s << "can't sync with"
                                       << visit->library << u"(format "_s + QString::number(format) + u"):"_s << "update Omabook on"
                                       << (format < theirs ? u"the hub."_s : computer + u"."_s);
            return std::unexpected(Failure(format < theirs ? Failure::Code::HubSyncHubOlder : Failure::Code::HubSyncHubNewer, words));
        });
    if (!checked)
        return std::unexpected(checked.error());
    if (!visit->scratch.isValid())
        return std::unexpected(Failure(Failure::Code::HubSyncNoPlace, {visit->scratch.path()}));
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    qCInfo(lcHub).noquote() << computer << (visit->dryRun ? "began a dry run of a sync of" : "began a sync of") << visit->library;
    const QString library = visit->library;
    m_visits.emplace(id, std::move(visit));
    return QJsonObject{{"visit", id}, {"library", library}, {"format", format}};
}

std::expected<QJsonObject, Failure> HubSync::step(Visit &visit, const QJsonObject &asked, HubKeeper &keeper) {
    const QString step = asked.value("step"_L1).toString();
    if (step == u"folder"_s)
        return visit.folder();
    if (step == u"put"_s)
        return visit.put(asked);
    if (step == u"fetch"_s)
        return visit.fetch(asked, keeper);
    if (step == u"omabook"_s)
        return visit.omabook(asked, keeper);
    if (step != u"drop"_s)
        return std::unexpected(Failure(Failure::Code::HubSyncRefused, {step.left(40)}));
    const auto folder = visit.resolve(asked.value("folder"_L1).toString());
    if (!folder)
        return std::unexpected(folder.error());
    QDir(*folder).removeRecursively();
    return QJsonObject();
}

std::expected<QJsonObject, Failure> HubSync::Visit::folder() {
    const QString name = u"f"_s + QString::number(++folders);
    if (!QDir().mkpath(scratch.filePath(name)))
        return std::unexpected(Failure(Failure::Code::HubSyncNoPlace, {name}));
    return QJsonObject{{"folder", name}};
}

std::expected<QJsonObject, Failure> HubSync::Visit::put(const QJsonObject &asked) const {
    const QString named = asked.value("file"_L1).toString();
    const auto file = resolve(named);
    // Into a folder of its own, made by a step before.
    const QString folder = file ? QFileInfo(*file).absolutePath() : QString();
    if (!file || !QFileInfo(folder).isDir() || folder == QDir(scratch.path()).absolutePath())
        return std::unexpected(Failure(Failure::Code::HubSyncNoPlace, {named.left(120)}));
    const QByteArray data = QByteArray::fromBase64(asked.value("data"_L1).toString().toLatin1());
    const qint64 at = asked.value("at"_L1).toInteger();
    QFile written(*file);
    // A file comes in order, a piece after the other; its first piece begins it anew.
    if ((at != 0 && at != written.size()) || data.size() > piece
        || !written.open(at == 0 ? QIODevice::WriteOnly | QIODevice::Truncate : QIODevice::Append) || written.write(data) != data.size())
        return std::unexpected(Failure(Failure::Code::HubSyncNoPlace, {named.left(120)}));
    return QJsonObject{{"size", written.size()}};
}

std::expected<QJsonObject, Failure> HubSync::Visit::fetch(const QJsonObject &asked, HubKeeper &keeper) {
    const QString asking = asked.value("file"_L1).toString();
    const auto inScratch = resolve(asking);
    QString file = inScratch && QFileInfo(*inScratch).isFile() ? *inScratch : QString();
    if (file.isEmpty()) {
        const auto known = given(asking, keeper);
        if (!known)
            return std::unexpected(known.error());
        file = *known;
    }
    QFile read(file);
    if (file.isEmpty() || !read.open(QIODevice::ReadOnly)) {
        qCWarning(lcHub).noquote() << computer << "asked its sync for a file the hub doesn't give:" << asking.left(120);
        return std::unexpected(Failure(Failure::Code::HubSyncNotGiven, {asking.left(120)}));
    }
    if (!read.seek(asked.value("at"_L1).toInteger()))
        return std::unexpected(Failure(Failure::Code::HubBadRequest));
    return QJsonObject{{"data", QString::fromLatin1(read.read(piece).toBase64())}, {"size", read.size()}};
}

std::expected<QString, Failure> HubSync::Visit::given(const QString &asking, HubKeeper &keeper) {
    QString file;
    const auto known = keeper.write(library, HubKeeper::Moment::None, [&](Library &kept) -> std::expected<QJsonObject, Failure> {
        if (!books)
            books = filesOf(kept);
        if (const QString found = canonical(asking); !found.isEmpty() && (books->contains(found) || pluginFileOf(kept, device, found)))
            file = found;
        return QJsonObject();
    });
    if (!known)
        return std::unexpected(known.error());
    return file;
}

std::expected<QJsonObject, Failure> HubSync::Visit::omabook(const QJsonObject &asked, HubKeeper &keeper) {
    QStringList arguments;
    for (const auto &argument : asked.value("arguments"_L1).toArray())
        arguments << argument.toString();
    const HubSyncSteps::Visit visit{.resolve = [this](const QString &named) { return resolve(named); }, .dryRun = dryRun, .device = device};
    // Its first step that writes takes the moment snapshot first, in the same lock (#181): none
    // for a sync the guard refuses (#187), or a dry run, which write nothing.
    // The plugins' files' steps write too (#212), and are checked the same.
    const bool writes = !dryRun && HubSyncSteps::writes(arguments, visit.resolve);
    // The sync guard asks the books' state before anything is written: a computer that writes without
    // it checks no plan, an Omabook from before the guard, and is to be updated first (#211).
    planned = planned || arguments == QStringList{u"manifest"_s, u"--state"_s};
    if (writes && !planned) {
        qCWarning(lcHub).noquote() << computer << "writes with no sync guard, an older Omabook: its sync of" << library
                                   << "is refused until it is updated.";
        return std::unexpected(Failure(Failure::Code::HubSyncUnguarded));
    }
    const bool first = writes && !snapshotted;
    const auto answered = keeper.write(
        library, first ? HubKeeper::Moment::Sync : HubKeeper::Moment::None, [&](Library &kept) -> std::expected<QJsonObject, Failure> {
            const auto done = HubSyncSteps::run(kept, arguments, visit);
            if (!done) {
                qCWarning(lcHub).noquote() << computer + u"'s sync of "_s + library << "was refused a step:" << done.error().english();
                return std::unexpected(done.error());
            }
            return QJsonObject{{"answer", *done}};
        });
    snapshotted = snapshotted || (first && answered);
    return answered;
}
