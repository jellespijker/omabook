#include "spacekeys.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <utility>

#include "secretfile.h"

using namespace Qt::StringLiterals;

namespace {

// The founding key's file, and the stem every later one's name starts with.
[[nodiscard]] QString foundingFile(const QString &folder, QStringView space) { return SpaceKey::fileFor(folder, space); }

[[nodiscard]] QString stem(const QString &folder, QStringView space) {
    const QString founding = QFileInfo(foundingFile(folder, space)).fileName();
    return founding.left(founding.size() - QStringView(u".key").size());
}

[[nodiscard]] QString laterFile(const QString &folder, QStringView space, const QString &keyId) {
    return QDir(folder).absoluteFilePath(u"%1.%2.key"_s.arg(stem(folder, space), keyId));
}

} // namespace

std::expected<std::vector<SpaceKey>, QString> SpaceKeys::all(const QString &folder, QStringView space) {
    std::vector<SpaceKey> keys;
    auto founding = SpaceKey::load(foundingFile(folder, space));
    if (!founding)
        return std::unexpected(founding.error());
    keys.push_back(std::move(*founding));
    for (const QString &name : QDir(folder).entryList({stem(folder, space) + u".*.key"_s}, QDir::Files, QDir::Name)) {
        auto later = SpaceKey::load(QDir(folder).absoluteFilePath(name));
        if (!later)
            return std::unexpected(later.error());
        keys.push_back(std::move(*later));
    }
    return keys;
}

std::expected<bool, QString> SpaceKeys::keep(const QString &folder, QStringView space, const SpaceKey &key) {
    const QString founding = foundingFile(folder, space);
    if (!QFileInfo::exists(founding)) {
        if (auto kept = key.keepIn(founding, SecretFile::Placing::KeepWhatIsThere); !kept)
            return std::unexpected(kept.error());
        return true;
    }
    if (const auto there = SpaceKey::load(founding); there && there->id() == key.id())
        return false;
    const QString later = laterFile(folder, space, key.id());
    if (QFileInfo::exists(later))
        return false;
    if (auto kept = key.keepIn(later, SecretFile::Placing::KeepWhatIsThere); !kept)
        return std::unexpected(kept.error());
    return true;
}

std::expected<qsizetype, QString> SpaceKeys::keepAll(const QString &folder, QStringView space, std::span<const SpaceKey> keys) {
    qsizetype added = 0;
    for (const SpaceKey &key : keys) {
        const auto kept = keep(folder, space, key);
        if (!kept)
            return std::unexpected(kept.error());
        added += *kept ? 1 : 0;
    }
    return added;
}
