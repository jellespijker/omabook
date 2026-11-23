#include "hubkeeper.h"

#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <iterator>
#include <ranges>
#include <utility>

#include "library.h"
#include "libraryhold.h"
#include "librarymigrations.h"
#include "log.h"

using namespace Qt::StringLiterals;

struct HubKeeper::Kept {
    QString name;
    // The hold outlives the library's connection: declared first, it goes last.
    std::unique_ptr<LibraryHold> hold;
    std::unique_ptr<Library> library;
};

class HubKeeper::Turn {
public:
    explicit Turn(HubKeeper &keeper) : m_keeper(keeper) {
        {
            const std::scoped_lock holding(m_keeper.m_holding);
            if (m_keeper.m_holder == std::this_thread::get_id()) {
                qCWarning(lcHub) << "Work on the hub's libraries asked for them again from inside; it could only wait for itself.";
                return;
            }
        }
        m_lock = std::unique_lock(m_keeper.m_lock);
        const std::scoped_lock holding(m_keeper.m_holding);
        m_keeper.m_holder = std::this_thread::get_id();
    }
    ~Turn() {
        if (!m_lock.owns_lock())
            return;
        const std::scoped_lock holding(m_keeper.m_holding);
        m_keeper.m_holder = {};
    }
    Turn(const Turn &) = delete;
    Turn &operator=(const Turn &) = delete;
    Turn(Turn &&) = delete;
    Turn &operator=(Turn &&) = delete;

    [[nodiscard]] bool had() const noexcept { return m_lock.owns_lock(); }

private:
    HubKeeper &m_keeper;
    std::unique_lock<std::mutex> m_lock;
};

QString HubKeeper::momentName(Moment moment) {
    static constexpr std::array names{"none", "sync", "import", "tidy", "merge", "restore", "upgrade", "bulk", "asked"};
    return QString::fromLatin1(names.at(static_cast<std::size_t>(moment)));
}

HubKeeper::HubKeeper(QString folder) : m_folder(std::move(folder)) {}

HubKeeper::~HubKeeper() = default;

std::expected<void, Failure> HubKeeper::open() {
    const Turn turn(*this);
    if (!turn.had())
        return std::unexpected(Failure(Failure::Code::LibraryBusy));
    // A library in an older format is brought up to date as it opens: a
    // write that changes much, so what comes before such a write comes first,
    // and refusing it leaves the library as it was, and the hub unopened (#208).
    const auto previous = LibraryMigrations::beforeUpgrading([this](Library &opening) -> std::expected<QString, Failure> {
        for (const Before &before : std::as_const(m_before))
            if (const auto done = before(opening, Moment::Upgrade); !done)
                return std::unexpected(Failure(Failure::Code::UpgradeNotSnapshot, {done.error().text()}));
        return QString();
    });
    const auto restore = qScopeGuard([&previous] { LibraryMigrations::beforeUpgrading(previous); });
    const QDir folder(m_folder);
    for (const QString &name : folder.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString root = folder.filePath(name);
        if (!QFileInfo::exists(Library::databaseIn(root)))
            continue;
        auto hold = LibraryHold::take(root);
        if (!hold)
            return std::unexpected(hold.error());
        auto library = std::make_unique<Library>(root);
        library->onBringingUpToDate([](const QString &said) { qCInfo(lcHub).noquote() << said; });
        QString error;
        if (!library->open(&error)) {
            qCWarning(lcHub).noquote() << "The library in" << root << "can't be kept:" << error;
            return std::unexpected(Failure::said(error));
        }
        qCInfo(lcHub).noquote() << "Keeping the library" << name << "in" << root;
        m_kept.push_back(std::make_unique<Kept>(Kept{.name = name, .hold = std::move(*hold), .library = std::move(library)}));
    }
    return {};
}

QStringList HubKeeper::names() const {
    QStringList names;
    for (const auto &kept : m_kept)
        names << kept->name;
    return names;
}

std::expected<Library *, Failure> HubKeeper::find(const QString &name) const {
    if (name.isEmpty() && m_kept.size() == 1)
        return m_kept.front()->library.get();
    if (name.isEmpty())
        return std::unexpected(m_kept.empty() ? Failure(Failure::Code::HubNoSuchLibrary, {QString()})
                                              : Failure(Failure::Code::HubLibraryUnnamed));
    const auto found = std::ranges::find(m_kept, name, [](const auto &kept) { return kept->name; });
    if (found == m_kept.end())
        return std::unexpected(Failure(Failure::Code::HubNoSuchLibrary, {name}));
    return (*found)->library.get();
}

std::expected<QJsonObject, Failure> HubKeeper::write(const QString &name, Moment moment, const Work &work) {
    const Turn turn(*this);
    if (!turn.had())
        return std::unexpected(Failure(Failure::Code::LibraryBusy));
    const auto library = find(name);
    if (!library)
        return std::unexpected(library.error());
    if (moment != Moment::None)
        for (const Before &before : m_before)
            if (auto done = before(**library, moment); !done) {
                qCWarning(lcHub).noquote() << "A" << momentName(moment) << "of" << (*library)->root()
                                           << "was refused before it began:" << done.error().english();
                return std::unexpected(done.error());
            }
    return work(**library);
}

std::expected<void, Failure> HubKeeper::whole(const Whole &work) {
    const Turn turn(*this);
    if (!turn.had())
        return std::unexpected(Failure(Failure::Code::LibraryBusy));
    std::vector<Library *> libraries;
    std::ranges::transform(m_kept, std::back_inserter(libraries), [](const auto &kept) { return kept->library.get(); });
    return work(libraries);
}

std::expected<std::unique_ptr<Library>, Failure> HubKeeper::copy(const QString &name, const QString &folder) {
    const Turn turn(*this);
    if (!turn.had())
        return std::unexpected(Failure(Failure::Code::LibraryBusy));
    const auto library = find(name);
    if (!library)
        return std::unexpected(library.error());
    if (QDir place(folder); !place.removeRecursively() || !QDir().mkpath(folder))
        return std::unexpected(Failure(Failure::Code::HubWorkNoFolder, {folder}));
    if (const auto copied = (*library)->copyInto(Library::databaseIn(folder)); !copied)
        return std::unexpected(Failure::said(copied.error()));
    return std::make_unique<Library>(folder);
}

bool HubKeeper::busy() const {
    const std::scoped_lock holding(m_holding);
    return m_holder != std::thread::id{};
}
