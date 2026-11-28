#include "snapshotdesk.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QtConcurrentRun>
#include <algorithm>
#include <expected>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>

#include "library.h"
#include "librarydiff.h"
#include "snapshotdestinations.h"
#include "snapshothealth.h"
#include "snapshotrestore.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] std::expected<std::unique_ptr<Library>, Failure> opened(const QString &root) {
    auto library = std::make_unique<Library>(root);
    if (QString why; !library->open(&why))
        return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {why}));
    return library;
}

} // namespace

struct SnapshotDesk::Listing {
    QVariantList snapshots, destinations;
    bool backedUp = false;
    QStringList warnings, otherLibraries;
    QString failure; // why the snapshots couldn't be listed
};

SnapshotDesk::SnapshotDesk(SnapshotPlace place, SnapshotContents contents, Root root, Warnings warnings, QObject *parent)
    : QObject(parent), m_place(std::move(place)), m_contents(std::move(contents)), m_root(std::move(root)), m_warn(std::move(warnings)) {}

SnapshotDesk::Warnings SnapshotDesk::health() {
    return [](const Snapshots &snapshots, const Library &library) {
        QStringList said;
        const SnapshotHealth health(snapshots);
        const QList<std::pair<QString, QString>> libraries{{Snapshots::libraryTag(library), QFileInfo(library.root()).fileName()}};
        for (const SnapshotWarnings::Warning &warning : health.warnings(libraries, QDateTime::currentDateTime()))
            said << warning.why.text();
        return said;
    };
}

bool SnapshotDesk::ready() const { return made().ready(); }

QString SnapshotDesk::repository() const { return ready() ? made().place().repository.location : SnapshotPlace::defaultRepository(); }

void SnapshotDesk::work(const QString &doing, std::function<Outcome()> job) {
    if (!m_doing.isEmpty())
        return;
    m_doing = doing;
    m_status.clear();
    m_failed = false;
    emit changed();
    std::ignore = QtConcurrent::run([this, job = std::move(job)] { // Its end is told through the call below, never waited for.
        const Outcome outcome = job();
        QMetaObject::invokeMethod(this, [this, outcome] {
            m_doing.clear();
            if (outcome.keep)
                outcome.keep(*this);
            m_status = outcome.said;
            m_failed = outcome.failed;
            emit changed();
        });
    });
}

SnapshotDesk::Listing SnapshotDesk::listing(const Snapshots &snapshots, const Library *library) const {
    Listing found;
    const SnapshotDestinations destinations(snapshots.place());
    for (const SnapshotDestination &destination : destinations.list())
        found.destinations << destinations.json(destination).toVariantMap();
    found.backedUp = destinations.backedUp();
    if (!library || !snapshots.ready())
        return found;
    if (m_warn)
        found.warnings = m_warn(snapshots, *library);
    const auto listed = snapshots.list(*library);
    if (!listed) {
        found.failure = listed.error().text();
        return found;
    }
    for (const Restic::Snapshot &snapshot : *listed | std::views::reverse)
        found.snapshots << SnapshotRestore::toJson(snapshot).toVariantMap();
    if (listed->isEmpty())
        if (const auto others = snapshots.elsewhere(*library))
            for (const Snapshots::Elsewhere &one : *others)
                found.otherLibraries << one.library;
    return found;
}

void SnapshotDesk::keep(const Listing &listing) {
    m_snapshots = listing.snapshots;
    m_destinations = listing.destinations;
    m_otherLibraries = listing.otherLibraries;
    m_backedUp = listing.backedUp;
    m_warnings = listing.warnings;
    m_listed = listing.failure.isEmpty();
}

void SnapshotDesk::refresh() {
    work(u"list"_s, [this, root = m_root(), snapshots = made()] {
        const auto library = opened(root);
        const Listing found = listing(snapshots, library ? library->get() : nullptr);
        if (!library)
            return Outcome{.said = library.error().text(), .failed = true, .keep = [found](SnapshotDesk &desk) { desk.keep(found); }};
        return Outcome{
            .said = found.failure, .failed = !found.failure.isEmpty(), .keep = [found](SnapshotDesk &desk) { desk.keep(found); }};
    });
}

void SnapshotDesk::setUp(const QString &repository, const QString &recoveryKey) {
    work(u"setup"_s, [this, root = m_root(), snapshots = made(), repository = repository.trimmed(), typed = recoveryKey.trimmed()] mutable {
        const auto key = snapshots.setUp(repository, typed);
        if (!key)
            return Outcome{.said = key.error().text(), .failed = true, .keep = {}};
        const auto library = opened(root);
        const Listing found = listing(snapshots, library ? library->get() : nullptr);
        // Opened with the key they were made with, there is no new key to show (#292).
        const bool opened = !typed.isEmpty() || snapshots.place().adopted.isValid();
        const QString location = snapshots.place().repository.location;
        return Outcome{.said =
                           opened ? tr("The snapshots in %1 are opened.").arg(location) : tr("Snapshots are set up in %1.").arg(location),
                       .failed = false,
                       .keep = [found, opened, key = *key](SnapshotDesk &desk) {
                           desk.keep(found);
                           if (!opened)
                               desk.m_recoveryKey = {{u"for"_s, QString()}, {u"key"_s, key}};
                       }};
    });
}

void SnapshotDesk::takeNow() {
    work(u"take"_s, [this, root = m_root(), snapshots = made()] {
        const auto library = opened(root);
        if (!library)
            return Outcome{.said = library.error().text(), .failed = true, .keep = {}};
        const auto taken = snapshots.take(**library, SnapshotReason::Asked);
        const Listing found = listing(snapshots, library->get());
        const auto keep = [found](SnapshotDesk &desk) { desk.keep(found); };
        if (!taken)
            return Outcome{.said = taken.error().text(), .failed = true, .keep = keep};
        return Outcome{.said = taken->unchanged ? tr("Nothing changed since snapshot %1, so none was taken.").arg(taken->id.left(8))
                                                : tr("Snapshot %1 taken.").arg(taken->id.left(8)),
                       .failed = false,
                       .keep = keep};
    });
}

void SnapshotDesk::compare(const QString &from, const QString &to) {
    work(u"diff"_s, [root = m_root(), snapshots = made(), from, to] {
        const auto library = opened(root);
        if (!library)
            return Outcome{.said = library.error().text(), .failed = true, .keep = {}};
        const auto compared = SnapshotRestore::compare(snapshots, **library, from, to);
        if (!compared)
            return Outcome{.said = compared.error().text(), .failed = true, .keep = {}};
        return Outcome{.said = {}, .failed = false, .keep = [diff = compared->toJson().toVariantMap()](SnapshotDesk &desk) {
                           desk.m_diff = diff;
                           desk.m_putBack.clear();
                       }};
    });
}

void SnapshotDesk::preview(const QString &snapshot, const QString &book) {
    work(u"preview"_s, [root = m_root(), snapshots = made(), snapshot, book] {
        const auto library = opened(root);
        if (!library)
            return Outcome{.said = library.error().text(), .failed = true, .keep = {}};
        const auto found = SnapshotRestore::find(snapshots, **library, snapshot);
        if (!found)
            return Outcome{.said = found.error().text(), .failed = true, .keep = {}};
        const auto copy = SnapshotRestore::Copy::open(snapshots, *found);
        if (!copy)
            return Outcome{.said = copy.error().text(), .failed = true, .keep = {}};
        const auto change = LibraryDiff::book(**library, copy->library(), book);
        if (!change)
            return Outcome{.said = Failure(Failure::Code::SnapshotBookNotFound, {book}).text(), .failed = true, .keep = {}};
        const QVariantMap putBack{{u"snapshot"_s, SnapshotRestore::toJson(*found).toVariantMap()},
                                  {u"book"_s, change->toJson().toVariantMap()}};
        return Outcome{.said = {}, .failed = false, .keep = [putBack](SnapshotDesk &desk) { desk.m_putBack = putBack; }};
    });
}

void SnapshotDesk::restoreBook(const QString &snapshot, const QString &book, bool details, bool annotations) {
    const QString title = m_putBack.value(u"book"_s).toMap().value(u"title"_s, m_restored.value(u"title"_s)).toString();
    work(u"restore"_s, [this, root = m_root(), snapshots = made(), snapshot, book, details, annotations, title] {
        const auto library = opened(root);
        if (!library)
            return Outcome{.said = library.error().text(), .failed = true, .keep = {}};
        const auto found = SnapshotRestore::find(snapshots, **library, snapshot);
        if (!found)
            return Outcome{.said = found.error().text(), .failed = true, .keep = {}};
        const auto copy = SnapshotRestore::Copy::open(snapshots, *found);
        if (!copy)
            return Outcome{.said = copy.error().text(), .failed = true, .keep = {}};
        const auto done = SnapshotRestore::book(snapshots, **library, *copy, book, {.details = details, .annotations = annotations});
        if (!done)
            return Outcome{.said = done.error().text(), .failed = true, .keep = {}};
        QVariantMap restored = done->toJson().toVariantMap();
        restored.insert(
            {{u"snapshot"_s, found->id}, {u"book"_s, book}, {u"title"_s, title}, {u"details"_s, details}, {u"annotations"_s, annotations}});
        const Listing listed = listing(snapshots, library->get());
        return Outcome{.said = tr("%1 is put back as snapshot %2 has it. A moment snapshot was taken first: %3, to undo it with.")
                                   .arg(title, found->shortId(), done->undo.left(8)),
                       .failed = false,
                       .keep = [listed, restored](SnapshotDesk &desk) {
                           desk.keep(listed);
                           desk.m_restored = restored;
                           desk.m_putBack.clear();
                           emit desk.booksChanged();
                       }};
    });
}

void SnapshotDesk::undoRestore() {
    if (m_restored.isEmpty())
        return;
    restoreBook(m_restored.value(u"undo"_s).toString(), m_restored.value(u"book"_s).toString(), m_restored.value(u"details"_s).toBool(),
                m_restored.value(u"annotations"_s).toBool());
}

void SnapshotDesk::restoreWhole(const QString &snapshot, const QString &folder) {
    work(u"whole"_s, [root = m_root(), snapshots = made(), snapshot, folder = folder.trimmed()] {
        const Library library(root);
        const auto found = SnapshotRestore::find(snapshots, library, snapshot);
        if (!found)
            return Outcome{.said = found.error().text(), .failed = true, .keep = {}};
        if (const auto done = SnapshotRestore::whole(snapshots, *found, folder); !done)
            return Outcome{.said = done.error().text(), .failed = true, .keep = {}};
        return Outcome{.said =
                           tr("The library as snapshot %1 holds it is restored into %2, a library of its own. The one in use is as it was.")
                               .arg(found->shortId(), folder),
                       .failed = false,
                       .keep = [folder](SnapshotDesk &desk) { desk.m_restoredInto = folder; }};
    });
}

void SnapshotDesk::addDestination(const QString &name, const QString &location, const QStringList &credentialNames,
                                  const QStringList &credentialValues) {
    SnapshotDestination asked{.name = name.trimmed(), .location = location.trimmed(), .credentials = {}};
    QList<std::pair<QString, QByteArray>> credentials;
    for (const auto &[credential, value] : std::views::zip(credentialNames, credentialValues)) {
        if (credential.trimmed().isEmpty())
            continue;
        asked.credentials << credential.trimmed();
        credentials.append({credential.trimmed(), value.toUtf8()});
    }
    work(u"add"_s, [this, root = m_root(), snapshots = made(), asked, credentials] {
        SnapshotDestinations destinations(snapshots.place());
        const auto key = destinations.add(asked, credentials);
        if (!key)
            return Outcome{.said = key.error().text(), .failed = true, .keep = {}};
        const auto library = opened(root);
        const Listing found = listing(snapshots, library ? library->get() : nullptr);
        return Outcome{.said = tr("The snapshots are copied to %1 too, every night.").arg(asked.name),
                       .failed = false,
                       .keep = [found, key = *key, name = asked.name](SnapshotDesk &desk) {
                           desk.keep(found);
                           desk.m_recoveryKey = {{u"for"_s, name}, {u"key"_s, key}};
                       }};
    });
}

void SnapshotDesk::testDestination(const QString &name) {
    work(u"test"_s, [snapshots = made(), name] {
        const auto tested = SnapshotDestinations(snapshots.place()).test(name);
        if (!tested)
            return Outcome{.said = tested.error().text(), .failed = true, .keep = [name](SnapshotDesk &desk) {
                               desk.m_tested = {{u"name"_s, name}, {u"reached"_s, false}};
                           }};
        QVariantMap answer{{u"name"_s, name}, {u"reached"_s, true}, {u"snapshots"_s, tested->snapshots}};
        if (tested->latest)
            answer.insert(u"latest"_s, SnapshotRestore::toJson(*tested->latest).toVariantMap());
        return Outcome{.said = tr("%1 answers, and holds %n snapshots.", "", static_cast<int>(tested->snapshots)).arg(name),
                       .failed = false,
                       .keep = [answer](SnapshotDesk &desk) { desk.m_tested = answer; }};
    });
}

void SnapshotDesk::removeDestination(const QString &name) {
    work(u"remove"_s, [this, root = m_root(), snapshots = made(), name] {
        if (const auto removed = SnapshotDestinations(snapshots.place()).remove(name); !removed)
            return Outcome{.said = removed.error().text(), .failed = true, .keep = {}};
        const auto library = opened(root);
        const Listing found = listing(snapshots, library ? library->get() : nullptr);
        return Outcome{.said = tr("%1 is no destination any more. What is kept there stays.").arg(name),
                       .failed = false,
                       .keep = [found](SnapshotDesk &desk) { desk.keep(found); }};
    });
}

void SnapshotDesk::forget() {
    m_recoveryKey.clear();
    m_putBack.clear();
    m_tested.clear();
    m_status.clear();
    m_failed = false;
    emit changed();
}
