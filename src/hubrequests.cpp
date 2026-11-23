#include "hubrequests.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <span>

#include "hubkeeper.h"
#include "library.h"

using namespace Qt::StringLiterals;

namespace {

// What the hub is and keeps, each library as it stands: asked by a computer
// before anything else, and by the Hubs panel (#189).
[[nodiscard]] std::expected<QJsonObject, Failure> status([[maybe_unused]] const QJsonObject &asked, HubKeeper &keeper) {
    QJsonArray libraries;
    const auto counted = keeper.whole([&libraries](std::span<Library *const> kept) -> std::expected<void, Failure> {
        for (const Library *library : kept)
            libraries.append(QJsonObject{
                {"name", QFileInfo(library->root()).fileName()}, {"books", library->books().size()}, {"format", library->formatVersion()}});
        return {};
    });
    if (!counted)
        return std::unexpected(counted.error());
    return QJsonObject{{"version", QString::fromLatin1(OMABOOK_VERSION)}, {"folder", keeper.folder()}, {"libraries", libraries}};
}

} // namespace

HubRequests HubRequests::usual() {
    HubRequests requests;
    requests.add(u"status"_s, status);
    return requests;
}

QStringList HubRequests::names() const {
    QStringList names;
    for (const auto &[name, answer] : m_answers)
        names << name;
    return names;
}

QJsonObject HubRequests::refusal(const Failure &why) {
    QJsonObject refused{{"ok", false}, {"error", why.english()}};
    if (why.code() == Failure::Code::HubSyncNotPaired)
        refused.insert("unpaired"_L1, why.arguments().value(0));
    return refused;
}

QJsonObject HubRequests::answer(const QJsonObject &asked, HubKeeper &keeper) const {
    const QString name = asked.value("request"_L1).toString();
    if (name.isEmpty())
        return refusal(Failure(Failure::Code::HubBadRequest));
    const auto found = m_answers.find(name);
    if (found == m_answers.end())
        return refusal(Failure(Failure::Code::HubUnknownRequest, {name}));
    const auto answered = found->second(asked, keeper);
    if (!answered)
        return refusal(answered.error());
    QJsonObject answer = *answered;
    answer.insert("ok"_L1, true);
    return answer;
}

QByteArray HubRequests::answerLine(const QByteArray &line, HubKeeper &keeper) const {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    const QJsonObject answered = error.error != QJsonParseError::NoError || !document.isObject()
                                     ? refusal(Failure(Failure::Code::HubBadRequest))
                                     : answer(document.object(), keeper);
    return QJsonDocument(answered).toJson(QJsonDocument::Compact) + '\n';
}
