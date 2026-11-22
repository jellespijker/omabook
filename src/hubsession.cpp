#include "hubsession.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>

#include "hubrequests.h"

using namespace Qt::StringLiterals;

namespace {

// Requests the session answers itself, beside the ones it hands on.
const QStringList &ownRequests() {
    static const QStringList own{u"computers"_s, u"hello"_s, u"unlink"_s};
    return own;
}

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

[[nodiscard]] QJsonObject succeeded(QJsonObject answer) {
    answer.insert("ok"_L1, true);
    return answer;
}

} // namespace

QJsonObject HubSession::hello() { return {{"request", "hello"}, {"protocol", protocol}}; }

const QStringList &HubSession::relayed() {
    static const QStringList relayed{u"status"_s};
    return relayed;
}

bool HubSession::asksForSession(QStringView command) {
    static const QRegularExpression session(u"^\\s*((\\S*/)?omabook\\s+hub\\s+session)?\\s*$"_s);
    return session.matchView(command).hasMatch();
}

QByteArray HubSession::answer(const QByteArray &line) {
    if (m_over)
        return compact(HubRequests::refusal(Failure(Failure::Code::HubProtocol, {QString::number(protocol)})));
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        const QJsonObject refused = HubRequests::refusal(Failure(Failure::Code::HubBadRequest));
        return compact(m_computer ? refused : ending(refused));
    }
    return compact(m_computer ? answerTo(document.object()) : greet(document.object()));
}

QJsonObject HubSession::ending(QJsonObject answer) {
    m_over = true;
    answer.insert("protocol"_L1, protocol);
    return answer;
}

QJsonObject HubSession::greet(const QJsonObject &asked) {
    if (asked.value("request"_L1).toString() != u"hello"_s || asked.value("protocol"_L1).toInt() != protocol)
        return ending(HubRequests::refusal(Failure(Failure::Code::HubProtocol, {QString::number(protocol)})));
    const auto computer = m_computers.byFingerprint(m_fingerprint);
    if (!computer || computer->identity != m_computers.owner()) {
        QJsonObject refused = HubRequests::refusal(Failure(computer ? Failure::Code::HubNotOwner : Failure::Code::HubNotAComputer));
        refused.insert("linked"_L1, false);
        return ending(refused);
    }
    m_computer = computer;
    QStringList requests = ownRequests() + relayed();
    requests.sort();
    return succeeded({{"protocol", protocol},
                      {"computer", computer->name},
                      {"version", QString::fromLatin1(OMABOOK_VERSION)},
                      {"requests", QJsonArray::fromStringList(requests)}});
}

QJsonObject HubSession::answerTo(const QJsonObject &asked) {
    // Unlinked meanwhile, from another session or on the hub itself: no more.
    const auto still = m_computers.byFingerprint(m_fingerprint);
    if (!still) {
        QJsonObject refused = HubRequests::refusal(Failure(Failure::Code::HubNotAComputer));
        refused.insert("linked"_L1, false);
        return ending(refused);
    }
    const QString name = asked.value("request"_L1).toString();
    if (name == u"hello"_s)
        return greet(asked);
    if (name == u"computers"_s) {
        QJsonArray computers;
        for (const HubComputers::Computer &computer : m_computers.computers())
            computers.append(QJsonObject{{"name", computer.name},
                                         {"fingerprint", computer.fingerprint},
                                         {"linked", computer.linked.toString(Qt::ISODate)},
                                         {"this", computer.fingerprint == m_fingerprint}});
        return succeeded({{"computers", computers}});
    }
    if (name == u"unlink"_s) {
        const auto gone = m_computers.unlink(m_fingerprint);
        if (!gone)
            return HubRequests::refusal(gone.error());
        return ending(succeeded({{"computer", gone->name}}));
    }
    if (!relayed().contains(name))
        return HubRequests::refusal(name.isEmpty() ? Failure(Failure::Code::HubBadRequest)
                                                   : Failure(Failure::Code::HubUnknownRequest, {name}));
    // Who asks is the key's computer, never what the line says.
    QJsonObject handed = asked;
    handed.insert("computer"_L1, still->name);
    const QByteArray answered = m_relay(compact(handed));
    const QJsonDocument document = QJsonDocument::fromJson(answered);
    return document.isObject() ? document.object() : HubRequests::refusal(Failure(Failure::Code::HubBadRequest));
}
