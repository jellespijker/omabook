#include "spaceswitch.h"

#include <QList>
#include <QStringList>
#include <optional>
#include <span>

#include "changelog.h"
#include "library.h"
#include "publishedlog.h"
#include "publishedspace.h"
#include "sharedspace.h"
#include "spaceparts.h"
#include "subscribedspace.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] bool isShared(const Spaces::Recorded &space) { return space.kind == QLatin1StringView(SharedSpace::kind); }
[[nodiscard]] bool isPublished(const Spaces::Recorded &space) { return space.kind == QLatin1StringView(PublishedSpace::kind); }

[[nodiscard]] std::optional<Spaces::Recorded> recordedAs(const Library &library, const QString &space) {
    for (const Spaces::Recorded &recorded : Spaces::recorded(library))
        if (recorded.id == space)
            return recorded;
    return std::nullopt;
}

[[nodiscard]] std::expected<SpaceSwitch::Switched, Failure> setShared(const Library &library, const Spaces::Recorded &space,
                                                                      const SpaceParts::Part &part, bool on) {
    SpaceSwitch::Switched switched{
        .space = space.id, .kind = space.kind, .name = space.name, .part = QString(part.name), .on = on, .changed = false};
    if (SharedSpace::state(library, space.id).parts.contains(part.name) == on)
        return switched;
    const ChangeLog::Entry entry{.space = space.id,
                                 .entity = u"space"_s,
                                 .id = space.id,
                                 .field = SpaceParts::switchField(part.name),
                                 .value = on ? u"1"_s : u"0"_s,
                                 .hlc = {}};
    if (const auto written = SharedSpace::write(library, space.id, std::span(&entry, 1)); !written)
        return std::unexpected(Failure::said(written.error()));
    switched.changed = true;
    return switched;
}

[[nodiscard]] std::expected<SpaceSwitch::Switched, Failure> setPublished(const Library &library, const Spaces::Recorded &space,
                                                                         const SpaceParts::Part &part, bool on) {
    std::optional<PublishedLog::Record> record = PublishedLog::recorded(library, space.id);
    if (SubscribedSpace::isSubscribed(library, space.id) || !record)
        return std::unexpected(Failure(Failure::Code::OnlyOwnerSwitches, {space.name}));
    SpaceSwitch::Switched switched{
        .space = space.id, .kind = space.kind, .name = space.name, .part = QString(part.name), .on = on, .changed = false};
    if (record->parts.contains(part.name) == on)
        return switched;
    record->parts.removeAll(part.name);
    if (on)
        record->parts << QString(part.name);
    PublishedLog::remember(library, space.id, *record);
    if (const auto followed = PublishedSpace::follow(library, space.id); !followed)
        return std::unexpected(Failure::said(followed.error()));
    switched.changed = true;
    return switched;
}

} // namespace

std::expected<Spaces::Recorded, Failure> SpaceSwitch::resolve(const Library &library, const QString &which) {
    QList<Spaces::Recorded> meant;
    for (const Spaces::Recorded &space : Spaces::recorded(library))
        if ((isShared(space) || isPublished(space))
            && (space.id == which.trimmed() || space.name.compare(which.simplified(), Qt::CaseInsensitive) == 0))
            meant << space;
    if (meant.isEmpty())
        return std::unexpected(Failure(Failure::Code::NoSuchSpace, {which}));
    if (meant.size() > 1)
        return std::unexpected(Failure(Failure::Code::SpaceNameTaken, {which}));
    return meant.first();
}

bool SpaceSwitch::isOn(const Library &library, const QString &space, const QString &part) {
    const auto record = recordedAs(library, space);
    if (!record)
        return false;
    if (isShared(*record))
        return SharedSpace::state(library, space).parts.contains(part);
    // What the owner of a space subscribed to switched off is taken back from it.
    if (SubscribedSpace::isSubscribed(library, space))
        return true;
    const auto published = PublishedLog::recorded(library, space);
    return published && published->parts.contains(part);
}

std::expected<SpaceSwitch::Switched, Failure> SpaceSwitch::set(const Library &library, const QString &which, const QString &part, bool on) {
    const SpaceParts::Part *known = SpaceParts::named(part);
    if (known == nullptr || !known->switched) {
        QStringList switched;
        for (const SpaceParts::Part &one : SpaceParts::all())
            if (one.switched)
                switched << QString(one.name);
        return std::unexpected(Failure(Failure::Code::PartNotSwitched, {part, switched.join(", "_L1)}));
    }
    const auto space = resolve(library, which);
    if (!space)
        return std::unexpected(space.error());
    return isShared(*space) ? setShared(library, *space, *known, on) : setPublished(library, *space, *known, on);
}
