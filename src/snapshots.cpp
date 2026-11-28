#include "snapshots.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRandomGenerator>
#include <QRegularExpression>
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
#include "libraryhold.h"
#include "log.h"
#include "scriptplugins.h"
#include "secretfile.h"
#include "settings.h"
#include "snapshotretention.h"

namespace {

using namespace Qt::StringLiterals;

// systemd's credential with the password, on a hub (LoadCredentialEncrypted=).
constexpr auto credential = "omabook-snapshots"_L1;
// The hub's secret with it (HubPlace::secret).
constexpr auto secretName = "snapshots"_L1;

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

// Keeps a new password: as the hub's secret on a hub (restic then has it
// handed over), else in its 0600 file.
[[nodiscard]] std::expected<void, Failure> keepPassword(const SnapshotPlace &place, const QString &key, ResticRepository &made) {
    const QByteArray bytes = key.toLatin1();
    if (place.hub) {
        if (const auto kept = place.hub->keepSecret(secretName, bytes); !kept)
            return std::unexpected(Failure(Failure::Code::SnapshotPasswordUnkept, {kept.error().text()}));
        made.password = bytes;
        return {};
    }
    if (const auto kept =
            SecretFile::keep(made.passwordFile, std::as_bytes(std::span(bytes.constData(), bytes.size())), SecretFile::Placing::Replace);
        !kept)
        return std::unexpected(Failure(Failure::Code::SnapshotPasswordUnkept, {kept.error()}));
    return {};
}

// The password, and whether it is a new one: what systemd handed over as a
// credential, or the file there is, else a new one, kept.
struct Password {
    QString key;
    bool made = false;
};

// Where this place's password is kept, as a person is told.
[[nodiscard]] QString passwordPlace(const SnapshotPlace &place) {
    return place.hub ? place.hub->state() + u"/secrets/"_s + secretName : place.repository.passwordFile;
}

// The password just made for a repository that wasn't made after all.
void forgetPassword(const SnapshotPlace &place, ResticRepository &made) {
    if (!place.hub) {
        QFile::remove(made.passwordFile);
        return;
    }
    place.hub->forgetSecret(secretName);
    made.password.clear();
}

[[nodiscard]] std::expected<Password, Failure> choosePassword(const SnapshotPlace &place, ResticRepository &made) {
    if (!made.password.isEmpty())
        return Password{.key = QString::fromUtf8(made.password), .made = false};
    if (QFile given(made.passwordFile); given.open(QIODevice::ReadOnly))
        return Password{.key = QString::fromUtf8(given.readAll()).trimmed(), .made = false};
    if (QFileInfo::exists(made.passwordFile)) // there, and not this user's to read
        return std::unexpected(Failure(Failure::Code::SnapshotPasswordUnkept, {made.passwordFile}));
    const QString key = recoveryKey();
    if (const auto kept = keepPassword(place, key, made); !kept)
        return std::unexpected(kept.error());
    return Password{.key = key, .made = true};
}

// A recovery key as a person types it: letters in either case, the dashes and
// spaces as they come. Anything else is a password of its own, taken as it is.
[[nodiscard]] QString typedKey(const QString &typed) {
    static const QRegularExpression between(u"[\\s-]"_s);
    static const QRegularExpression key(u"^[A-HJ-NP-Z2-9]{32}$"_s);
    const QString letters = typed.toUpper().remove(between);
    if (!key.match(letters).hasMatch())
        return typed.trimmed();
    QStringList groups;
    for (qsizetype i = 0; i < letters.size(); i += 4)
        groups << letters.mid(i, 4);
    return groups.join(u'-');
}

// Whether what is kept at `repository` opens with `key`; nothing is made or changed.
[[nodiscard]] std::expected<void, Failure> opens(ResticRepository repository, const QString &key) {
    repository.password = key.toUtf8();
    return Restic::answers(repository, std::chrono::minutes(5));
}

// Whether restic refused to make a repository because there is one already.
[[nodiscard]] bool thereAlready(const Failure &failure) {
    const QString said = failure.arguments().value(0);
    return failure.code() == Failure::Code::ResticFailed && (said.contains("already exists"_L1) || said.contains("already initialized"_L1));
}

[[nodiscard]] QString hashOf(QByteArrayView bytes, int length = 16) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(length));
}

using Plan = Snapshots::Staged;

[[nodiscard]] Plan planFor(const Library &library, const SnapshotContents &contents, const QString &staging) {
    const QString root = library.root();
    Plan plan{.root = root,
              .tag = Snapshots::libraryTag(library),
              .folder = staging,
              .paths = {root, staging},
              .leftOut = {},
              .leftOutNames = {u".details.lock"_s, QString::fromLatin1(LibraryHold::fileName)}};
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
        if (path == plan.folder || QFileInfo(path).isFile()) {
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
        if (file == plan.folder) {
            for (const QString &staged : {u"omabook.db"_s, u"snapshot.json"_s}) {
                QFile copy(plan.folder + u'/' + staged);
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
    if (config.open(QIODevice::ReadOnly)) {
        const QJsonObject choices = QJsonDocument::fromJson(config.readAll()).object();
        place.repository.location = choices.value("repository"_L1).toString();
        place.adopted = QDateTime::fromString(choices.value("adopted"_L1).toString(), Qt::ISODate);
    }
    place.repository.passwordFile = passwordIn(state);
    return place;
}

SnapshotPlace SnapshotPlace::ofHub(const HubPlace &hub) {
    SnapshotPlace place = in(hub.state() + u"/snapshots"_s);
    place.hub = hub;
    place.repository.passwordFile.clear();
    place.repository.password = hub.secret(secretName).value_or(QByteArray());
    return place;
}

QString SnapshotPlace::defaultRepository() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/omabook/snapshots"_s;
}

SnapshotContents SnapshotContents::here() {
    return {.settingsFile = Settings::defaultFile(), .pluginsFolder = ScriptPlugins::defaultFolder()};
}

Snapshots::Snapshots(SnapshotPlace place, SnapshotContents contents) : m_place(std::move(place)), m_contents(std::move(contents)) {}

bool Snapshots::ready() const {
    return !m_place.repository.location.isEmpty()
           && (!m_place.repository.password.isEmpty() || QFileInfo::exists(m_place.repository.passwordFile));
}

std::expected<QString, Failure> Snapshots::setUp(const QString &repository, const QString &recoveryKey) {
    if (ready())
        return std::unexpected(Failure(Failure::Code::SnapshotsSetUpAlready, {m_place.repository.location}));
    const QString chosen = m_place.repository.location;
    QString location = repository.isEmpty() ? SnapshotPlace::defaultRepository() : repository;
    // A folder on this computer, unless restic is named a kind of place (sftp:, s3:, rclone:...).
    if (QDir::isAbsolutePath(location) || !location.contains(u':'))
        location = QDir(location).absolutePath();
    if (repository.isEmpty() && !chosen.isEmpty())
        location = chosen;
    if (!QDir().mkpath(m_place.state))
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {m_place.state}));
    ResticRepository made = m_place.repository;
    made.location = location;
    if (!recoveryKey.trimmed().isEmpty())
        return openThere(made, typedKey(recoveryKey));
    // Set up already, and only the password gone: nothing new is made over it (#296).
    if (location == chosen)
        return std::unexpected(Failure(Failure::Code::SnapshotPasswordMissing, {chosen, passwordPlace(m_place)}));
    return makeThere(made);
}

std::expected<QString, Failure> Snapshots::makeThere(ResticRepository made) {
    const auto password = choosePassword(m_place, made);
    if (!password)
        return std::unexpected(password.error());
    QString key = password->key;
    QDateTime adopted;
    if (const auto initialised = Restic::init(made); !initialised) {
        if (password->made)
            forgetPassword(m_place, made);
        if (!thereAlready(initialised.error()))
            return std::unexpected(initialised.error());
        // Snapshots already: opened when the password kept here is theirs, and otherwise said.
        if (password->made || !opens(made, key))
            return std::unexpected(Failure(Failure::Code::SnapshotRepositoryExists, {made.location}));
        adopted = QDateTime::currentDateTimeUtc();
    }
    return chose(made, adopted).transform([&key] { return key; });
}

std::expected<QString, Failure> Snapshots::openThere(ResticRepository made, const QString &key) {
    // A password kept here already may be all that opens other snapshots: never written over.
    QByteArray kept = made.password;
    if (kept.isEmpty() && QFileInfo::exists(made.passwordFile)) {
        QFile file(made.passwordFile);
        if (!file.open(QIODevice::ReadOnly))
            return std::unexpected(Failure(Failure::Code::SnapshotPasswordUnkept, {made.passwordFile}));
        kept = file.readAll().trimmed();
    }
    if (!kept.isEmpty() && QString::fromUtf8(kept) != key)
        return std::unexpected(Failure(Failure::Code::SnapshotPasswordOther, {passwordPlace(m_place)}));
    if (const auto opened = opens(made, key); !opened)
        return std::unexpected(Failure(Failure::Code::SnapshotKeyRefused, {made.location, opened.error().text()}));
    if (kept.isEmpty())
        if (const auto done = keepPassword(m_place, key, made); !done)
            return std::unexpected(done.error());
    // This computer's own, their password put back, are thinned as before.
    const QDateTime adopted = made.location == m_place.repository.location ? m_place.adopted : QDateTime::currentDateTimeUtc();
    return chose(made, adopted).transform([&key] { return key; });
}

std::expected<void, Failure> Snapshots::chose(const ResticRepository &made, const QDateTime &adopted) {
    QJsonObject choices{{u"repository"_s, made.location}};
    if (adopted.isValid())
        choices.insert(u"adopted"_s, adopted.toString(Qt::ISODate));
    QSaveFile config(configOf(m_place.state));
    if (!config.open(QIODevice::WriteOnly) || config.write(QJsonDocument(choices).toJson()) <= 0 || !config.commit())
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {m_place.state}));
    m_place.repository = made;
    m_place.adopted = adopted;
    qCInfo(lcLibrary).noquote() << "snapshots set up in" << made.location;
    return {};
}

std::expected<QList<Snapshots::Elsewhere>, Failure> Snapshots::elsewhere(const Library &library) const {
    if (!ready())
        return std::unexpected(Failure(Failure::Code::SnapshotsNotSetUp));
    const auto all = Restic::snapshots(m_place.repository, {u"omabook"_s});
    if (!all)
        return std::unexpected(all.error());
    const QString own = libraryTag(library).section(u':', 1);
    QMap<QString, Elsewhere> found; // by folder
    for (const Restic::Snapshot &snapshot : *all) {
        const QString tag = snapshot.tagged(u"library:"_s);
        // The folder the tag was made of is one of the paths the snapshot holds.
        const auto folder = std::ranges::find_if(snapshot.paths, [&tag](const QString &path) { return hashOf(path.toUtf8()) == tag; });
        if (tag == own || folder == snapshot.paths.end())
            continue;
        Elsewhere &one = found[*folder];
        one.library = *folder;
        ++one.snapshots;
        one.newest = std::max(one.newest, snapshot.time);
    }
    return found.values();
}

std::expected<Snapshots::Staged, Failure> Snapshots::stage(const Library &library) const {
    if (!ready())
        return std::unexpected(Failure(Failure::Code::SnapshotsNotSetUp));
    const QString staging = m_place.state + u"/staging/"_s + libraryTag(library).section(u':', 1);
    QDir(staging).removeRecursively();
    auto cleanUp = qScopeGuard([&staging] { QDir(staging).removeRecursively(); });
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
    cleanUp.dismiss();
    return planFor(library, m_contents, staging);
}

std::expected<Snapshots::Taken, Failure> Snapshots::keep(const Staged &staged, SnapshotReason reason) const {
    const auto cleanUp = qScopeGuard([&staged] { QDir(staged.folder).removeRecursively(); });
    const QString state = stateOf(staged);
    const bool moment = reason != SnapshotReason::Nightly;
    if (moment) {
        const auto earlier = listOf(staged.tag);
        if (!earlier)
            return std::unexpected(earlier.error());
        if (!earlier->isEmpty() && earlier->last().tagged(u"state:"_s) == state)
            return Taken{.id = earlier->last().id, .unchanged = true};
    }
    const QStringList tags{u"omabook"_s, staged.tag, moment ? u"moment"_s : u"nightly"_s, u"reason:"_s + reasonName(reason),
                           u"state:"_s + state};
    const auto made = Restic::backup(m_place.repository, backupOf(staged, tags));
    if (!made)
        return std::unexpected(made.error());
    qCInfo(lcLibrary).noquote() << "snapshot" << made->left(8) << "of" << staged.root << "before" << reasonName(reason);
    return Taken{.id = *made, .unchanged = false};
}

std::expected<Snapshots::Taken, Failure> Snapshots::take(const Library &library, SnapshotReason reason) const {
    const auto staged = stage(library);
    if (!staged)
        return std::unexpected(staged.error());
    return keep(*staged, reason);
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

std::expected<QList<Restic::Snapshot>, Failure> Snapshots::list(const Library &library) const { return listOf(libraryTag(library)); }

std::expected<QList<Restic::Snapshot>, Failure> Snapshots::listOf(const QString &tag) const {
    if (!ready())
        return std::unexpected(Failure(Failure::Code::SnapshotsNotSetUp));
    return Restic::snapshots(m_place.repository, {u"omabook"_s, tag});
}

std::expected<qsizetype, Failure> Snapshots::thin(const Library &library, const QDateTime &now) const {
    return thin(libraryTag(library), now);
}

std::expected<qsizetype, Failure> Snapshots::thin(const QString &tag, const QDateTime &now) const {
    const auto snapshots = listOf(tag);
    if (!snapshots)
        return std::unexpected(snapshots.error());
    QList<SnapshotRetention::Dated> dated;
    for (const Restic::Snapshot &snapshot : *snapshots)
        // What was there when this computer opened these snapshots stays (#292).
        if (!m_place.adopted.isValid() || snapshot.time >= m_place.adopted)
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

QString Snapshots::newRecoveryKey() { return recoveryKey(); }

QString Snapshots::libraryTag(const Library &library) { return u"library:"_s + hashOf(library.root().toUtf8()); }
