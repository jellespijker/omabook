#include "hubclaims.h"

#include <QJsonObject>
#include <QJsonValue>

#include "plugindata.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] PluginData::Thing thingOf(const QString &book) {
    return {.plugin = u"omabook-hub"_s, .entity = u"graph-claim"_s, .id = book};
}

} // namespace

std::optional<HubClaims::Claim> HubClaims::of(const Library &library, const QString &book) {
    const QJsonObject fields = PluginData::read(library, thingOf(book)).toObject().value("fields"_L1).toObject();
    const QString by = fields.value("by"_L1).toString();
    if (by.isEmpty())
        return std::nullopt;
    return Claim{.by = by, .at = QDateTime::fromString(fields.value("at"_L1).toString(), Qt::ISODateWithMs)};
}

bool HubClaims::elsewhere(const Library &library, const QString &book, const QDateTime &now) {
    const auto claim = of(library, book);
    if (!claim || claim->by == Spaces::deviceId(library))
        return false;
    // A claim that says no time holds nothing; one that was made holds for its lease.
    return claim->at.isValid() && now < claim->at.addSecs(std::chrono::duration_cast<std::chrono::seconds>(lease).count());
}

std::expected<void, Failure> HubClaims::claim(const Library &library, const QString &book, const QDateTime &now) {
    const auto written =
        PluginData::set(library, thingOf(book), {{"by", Spaces::deviceId(library)}, {"at", now.toUTC().toString(Qt::ISODateWithMs)}});
    if (!written)
        return std::unexpected(Failure::said(written.error()));
    return {};
}
