#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <expected>
#include <functional>
#include <map>

#include "failure.h"

class HubKeeper;

// What a hub's daemon is asked over its socket (ADR 0012, 0013): a fixed set
// of requests, each by its name, and nothing else. A computer's session asks
// through `omabook hub session`, the hub's own jobs ask the same way; each
// answer is worked out with the keeper (HubKeeper), which hands the libraries
// to one request at a time.
//
// A request is a JSON object: {"request": "status"}, with what that request
// takes beside it ("library" names one of the libraries). An answer is one
// too: {"ok": true, ...} with what it answers, or {"ok": false, "error": the
// failure in English}.
//
// #179 brings `status`; the ones to come (#180 on: snapshots, restore, logs,
// sync) are added to the table as they are built, through add().
class HubRequests {
public:
    // How one request is answered, with the keeper to do it through.
    using Answer = std::function<std::expected<QJsonObject, Failure>(const QJsonObject &asked, HubKeeper &keeper)>;

    // The hub's own requests.
    [[nodiscard]] static HubRequests usual();

    void add(const QString &name, Answer answer) { m_answers.insert_or_assign(name, std::move(answer)); }
    [[nodiscard]] QStringList names() const;

    // The answer to `asked`, refused when it is no request of the set.
    [[nodiscard]] QJsonObject answer(const QJsonObject &asked, HubKeeper &keeper) const;
    // The answer to one line a socket sent, as one line to send back.
    [[nodiscard]] QByteArray answerLine(const QByteArray &line, HubKeeper &keeper) const;

    // An answer that says why not.
    [[nodiscard]] static QJsonObject refusal(const Failure &why);

private:
    std::map<QString, Answer> m_answers;
};
