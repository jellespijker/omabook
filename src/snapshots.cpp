#include "snapshots.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <utility>

#include "library.h"
#include "log.h"
#include "scriptplugins.h"
#include "secretfile.h"
#include "settings.h"
#include "snapshotretention.h"

namespace {

using namespace Qt::StringLiterals;

// systemd's credential with the password, on a hub (LoadCredentialEncrypted=).
constexpr auto credential = "omabook-snapshots"_L1;

[[nodiscard]] QString configOf(const QString &state) { return state + u"/snapshots.json"_s; }

[[nodiscard]] QString passwordIn(const QString &state) {
    if (const QString credentials = qEnvironmentVariable("CREDENTIALS_DIRECTORY"); !credentials.isEmpty()) {
        if (const QString file = credentials + u'/' + credential; QFileInfo::exists(file))
            return file;
    }
    return state + u"/password"_s;
}

// The password, and the recovery key: 160 random bits as 32 letters and
// digits that can't be mistaken for each other, in groups of four.
[[nodiscard]] QString recoveryKey() {
    QString letters;
    for (char letter = 'A'; letter <= 'Z'; ++letter)
        if (letter != 'I' && letter != 'O')
            letters += QChar::fromLatin1(letter);
    for (char digit = '2'; digit <= '9'; ++digit)
        letters += QChar::fromLatin1(digit);
    QString key;
    for (int i = 0; i < 32; ++i) {
        if (i > 0 && i % 4 == 0)
            key += u'-';
        key += letters.at(QRandomGenerator::system()->bounded(static_cast<int>(letters.size())));
    }
    return key;
}

[[nodiscard]] QString hashOf(QByteArrayView bytes, int length = 16) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(length));
}

// What a snapshot holds of one library, and what restic is to leave out.
struct Plan {
    QString staging;          // where the database's copy and the manifest wait
    QStringList paths;        // what restic keeps
    QStringList leftOut;      // whole paths left out of those
    QStringList leftOutNames; // files left out wherever they are
};

[[nodiscard]] Plan planFor(const Library &library, const SnapshotContents &contents, const QString &staging) {
    const QString root = library.root();
    Plan plan{.staging = staging, .paths = {root, staging}, .leftOut = {}, .leftOutNames = {u".details.lock"_s}};
    // The database is kept as its copy; the format copies are older databases.
    for (const QString &file :
         {library.databasePath(), library.databasePath() + u"-wal"_s, library.databasePath() + u"-shm"_s, library.backupFolder()})
        plan.leftOut << file;
    if (QFileInfo(contents.settingsFile).isFile())
        plan.paths << contents.settingsFile;
    for (const QFileInfo &plugin : QDir(contents.pluginsFolder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (const QString data = plugin.filePath() + u"/data"_s; QFileInfo(data).isDir())
            plan.paths << data;
    return plan;
}

// What the snapshot would hold, in a few letters: the copy's bytes, and each
// file's path, size and time. The same as the last one's, nothing changed.
[[nodiscard]] QString stateOf(const Plan &plan) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QStringList files;
    for (const QString &path : plan.paths) {
        if (path == plan.staging || QFileInfo(path).isFile()) {
            files << path;
            continue;
        }
        QDirIterator walk(path, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (walk.hasNext())
            if (const QString file = walk.next();
                !plan.leftOutNames.contains(walk.fileName())
                && std::ranges::none_of(plan.leftOut, [&file](const QString &out) { return file == out || file.startsWith(out + u'/'); }))
                files << file;
    }
    std::ranges::sort(files);
    for (const QString &file : files) {
        if (file == plan.staging) {
            for (const QString &staged : {u"omabook.db"_s, u"snapshot.json"_s}) {
                QFile copy(plan.staging + u'/' + staged);
                if (copy.open(QIODevice::ReadOnly))
                    hash.addData(&copy);
            }
            continue;
        }
        const QFileInfo info(file);
        hash.addData(u"%1\n%2\n%3\n"_s.arg(file).arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch()).toUtf8());
    }
    return QString::fromLatin1(hash.result().toHex().left(16));
}

[[nodiscard]] Restic::Backup backupOf(const Plan &plan, const QStringList &tags) {
    Restic::Backup backup{.paths = plan.paths, .excluding = {}, .tags = tags};
    for (const QString &path : plan.leftOut)
        backup.excluding << Restic::literal(path);
    for (const QString &name : plan.leftOutNames)
        backup.excluding << Restic::literal(name);
    return backup;
}

// Where the snapshot came from, beside the database's copy: what a restore
// (#183) puts back where. Only what stays the same while nothing changes.
[[nodiscard]] bool writeManifest(const Library &library, const SnapshotContents &contents, const QString &staging) {
    QSaveFile file(staging + u"/snapshot.json"_s);
    const QJsonObject manifest{{u"library"_s, library.root()},
                               {u"database"_s, u"omabook.db"_s},
                               {u"format"_s, library.formatVersion()},
                               {u"settings"_s, contents.settingsFile},
                               {u"plugins"_s, contents.pluginsFolder}};
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(manifest).toJson()) > 0 && file.commit();
}

} // namespace

SnapshotPlace SnapshotPlace::here() {
    const QString states =
        qEnvironmentVariableIsSet("XDG_STATE_HOME") ? qEnvironmentVariable("XDG_STATE_HOME") : QDir::homePath() + u"/.local/state"_s;
    return in(states + u"/omabook/snapshots"_s);
}

SnapshotPlace SnapshotPlace::in(const QString &state) {
    SnapshotPlace place{.state = state, .repository = {}};
    QFile config(configOf(state));
    if (config.open(QIODevice::ReadOnly))
        place.repository.location = QJsonDocument::fromJson(config.readAll()).object().value("repository"_L1).toString();
    place.repository.passwordFile = passwordIn(state);
    return place;
}

QString SnapshotPlace::defaultRepository() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/omabook/snapshots"_s;
}

SnapshotContents SnapshotContents::here() {
    return {.settingsFile = Settings::defaultFile(), .pluginsFolder = ScriptPlugins::defaultFolder()};
}

Snapshots::Snapshots(SnapshotPlace place, SnapshotContents contents) : m_place(std::move(place)), m_contents(std::move(contents)) {}

bool Snapshots::ready() const { return !m_place.repository.location.isEmpty() && QFileInfo::exists(m_place.repository.passwordFile); }

std::expected<QString, Failure> Snapshots::setUp(const QString &repository) {
    if (ready())
        return std::unexpected(Failure(Failure::Code::SnapshotsSetUpAlready, {m_place.repository.location}));
    QString location = repository.isEmpty() ? SnapshotPlace::defaultRepository() : repository;
    // A folder on this computer, unless restic is named a kind of place (sftp:, s3:, rclone:...).
    if (QDir::isAbsolutePath(location) || !location.contains(u':'))
        location = QDir(location).absolutePath();
    if (!QDir().mkpath(m_place.state))
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {m_place.state}));
    ResticRepository made = m_place.repository;
    made.location = location;
    // A hub's credential is its own password; here one is made.
    const bool ownPassword = !QFileInfo::exists(made.passwordFile);
    QString key;
    if (ownPassword) {
        key = recoveryKey();
        const QByteArray bytes = key.toLatin1();
        if (const auto kept = SecretFile::keep(made.passwordFile, std::as_bytes(std::span(bytes.constData(), bytes.size())),
                                               SecretFile::Placing::Replace);
            !kept)
            return std::unexpected(Failure(Failure::Code::SnapshotPasswordUnkept, {kept.error()}));
    } else if (QFile given(made.passwordFile); given.open(QIODevice::ReadOnly)) {
        key = QString::fromUtf8(given.readAll()).trimmed();
    }
    if (const auto initialised = Restic::init(made); !initialised) {
        if (ownPassword)
            QFile::remove(made.passwordFile);
        return std::unexpected(initialised.error());
    }
    QSaveFile config(configOf(m_place.state));
    if (!config.open(QIODevice::WriteOnly) || config.write(QJsonDocument(QJsonObject{{u"repository"_s, location}}).toJson()) <= 0
        || !config.commit())
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {m_place.state}));
    m_place.repository = made;
    qCInfo(lcLibrary).noquote() << "snapshots set up in" << location;
    return key;
}

std::expected<Snapshots::Taken, Failure> Snapshots::take(const Library &library, SnapshotReason reason) const {
    if (!ready())
        return std::unexpected(Failure(Failure::Code::SnapshotsNotSetUp));
    const QString tag = libraryTag(library);
    const QString staging = m_place.state + u"/staging/"_s + tag.section(u':', 1);
    QDir(staging).removeRecursively();
    const auto cleanUp = qScopeGuard([&staging] { QDir(staging).removeRecursively(); });
    if (!QDir().mkpath(staging))
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {staging}));
    {
        // In the library's write turn: every writer before is done, none after has begun.
        const Library::Batch batch(library);
        if (!batch.begun())
            return std::unexpected(Failure(Failure::Code::LibraryBusy));
        if (const auto copied = library.copyInto(staging + u"/omabook.db"_s); !copied)
            return std::unexpected(Failure(Failure::Code::SnapshotNotCopied, {copied.error()}));
    }
    if (!writeManifest(library, m_contents, staging))
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {staging}));
    const Plan plan = planFor(library, m_contents, staging);
    const QString state = stateOf(plan);
    const bool moment = reason != SnapshotReason::Nightly;
    if (moment) {
        const auto earlier = list(library);
        if (!earlier)
            return std::unexpected(earlier.error());
        if (!earlier->isEmpty() && earlier->last().tagged(u"state:"_s) == state)
            return Taken{.id = earlier->last().id, .unchanged = true};
    }
    const QStringList tags{u"omabook"_s, tag, moment ? u"moment"_s : u"nightly"_s, u"reason:"_s + reasonName(reason), u"state:"_s + state};
    const auto made = Restic::backup(m_place.repository, backupOf(plan, tags));
    if (!made)
        return std::unexpected(made.error());
    qCInfo(lcLibrary).noquote() << "snapshot" << made->left(8) << "of" << library.root() << "before" << reasonName(reason);
    return Taken{.id = *made, .unchanged = false};
}

std::expected<std::optional<Snapshots::Taken>, Failure> Snapshots::before(const Library &library, SnapshotReason moment) const {
    if (!ready())
        return std::nullopt;
    return take(library, moment).transform([](const Taken &taken) { return std::optional(taken); });
}

std::expected<std::optional<Snapshots::Taken>, Failure> Snapshots::nightly(const Library &library, const QDateTime &now) const {
    if (!ready())
        return std::nullopt;
    const auto earlier = list(library);
    if (!earlier)
        return std::unexpected(earlier.error());
    std::optional<QDateTime> last;
    for (const Restic::Snapshot &snapshot : *earlier)
        if (snapshot.tags.contains(u"nightly"_s))
            last = snapshot.time;
    if (!SnapshotRetention::nightlyDue(last, now))
        return std::nullopt;
    const auto taken = take(library, SnapshotReason::Nightly);
    if (!taken)
        return std::unexpected(taken.error());
    if (const auto thinned = thin(library, now); !thinned)
        return std::unexpected(thinned.error());
    return *taken;
}

std::expected<QList<Restic::Snapshot>, Failure> Snapshots::list(const Library &library) const {
    if (!ready())
        return std::unexpected(Failure(Failure::Code::SnapshotsNotSetUp));
    return Restic::snapshots(m_place.repository, {u"omabook"_s, libraryTag(library)});
}

std::expected<qsizetype, Failure> Snapshots::thin(const Library &library, const QDateTime &now) const {
    const auto snapshots = list(library);
    if (!snapshots)
        return std::unexpected(snapshots.error());
    QList<SnapshotRetention::Dated> dated;
    for (const Restic::Snapshot &snapshot : *snapshots)
        dated << SnapshotRetention::Dated{.id = snapshot.id, .time = snapshot.time};
    const QStringList forgotten = SnapshotRetention::toForget(dated, now);
    if (const auto done = Restic::forget(m_place.repository, forgotten); !done)
        return std::unexpected(done.error());
    return forgotten.size();
}

QString Snapshots::reasonName(SnapshotReason reason) {
    static constexpr std::array names{"nightly", "asked", "sync", "import", "tidy", "merge", "restore", "upgrade", "bulk-change"};
    return QString::fromLatin1(names.at(static_cast<std::size_t>(reason)));
}

QString Snapshots::libraryTag(const Library &library) { return u"library:"_s + hashOf(library.root().toUtf8()); }
