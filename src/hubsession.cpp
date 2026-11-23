#include "hubsession.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>

#include "hubrequests.h"
#include "log.h"

using namespace Qt::StringLiterals;

namespace {

// Requests the session answers itself, beside the ones it hands on.
const QStringList &ownRequests() {
    static const QStringList own{u"computers"_s, u"forget"_s, u"hello"_s, u"unlink"_s};
    return own;
}

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// The longest piece of what a client sent that the log keeps.
constexpr qsizetype longestShown = 120;

[[nodiscard]] QJsonObject succeeded(QJsonObject answer) {
    answer.insert("ok"_L1, true);
    return answer;
}

} // namespace

QString HubSession::shown(QStringView said) {
    QString kept = said.left(longestShown).toString();
    // One line of the log for each thing that happened, whatever was sent.
    for (QChar &c : kept)
        if (c.category() == QChar::Other_Control)
            c = u' ';
    return said.size() > longestShown ? kept + u"..."_s : kept;
}

QJsonObject HubSession::hello() { return {{"request", "hello"}, {"protocol", protocol}}; }

const QStringList &HubSession::relayed() {
    static const QStringList relayed{u"snapshots"_s, u"status"_s, u"sync"_s};
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
        qCWarning(lcHub).noquote() << "A session for the key" << m_fingerprint
                                   << u"sent a line that is no JSON object ("_s + shown(QString::fromUtf8(line))
                                          + (m_computer ? u"): refused."_s : u"): refused, and the session ended."_s);
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
    if (asked.value("request"_L1).toString() != u"hello"_s || asked.value("protocol"_L1).toInt() != protocol) {
        qCWarning(lcHub).noquote() << "A session for the key" << m_fingerprint << "began with something other than a hello in protocol"
                                   << protocol
                                   << u"("_s + shown(QString::fromUtf8(QJsonDocument(asked).toJson(QJsonDocument::Compact)))
                                          + "): refused, and the session ended.";
        return ending(HubRequests::refusal(Failure(Failure::Code::HubProtocol, {QString::number(protocol)})));
    }
    const auto computer = m_computers.byFingerprint(m_fingerprint);
    if (!computer || computer->identity != m_computers.owner()) {
        qCWarning(lcHub).noquote() << "A session for the key" << m_fingerprint
                                   << (computer ? "of a computer not the hub owner's" : "that isn't linked to this hub")
                                   << "said hello: refused.";
        QJsonObject refused = HubRequests::refusal(Failure(computer ? Failure::Code::HubNotOwner : Failure::Code::HubNotAComputer));
        refused.insert("linked"_L1, false);
        return ending(refused);
    }
    m_computer = computer;
    noteClaim(asked, computer->name);
    qCInfo(lcHub).noquote() << "A session began for" << computer->name << u"("_s + m_fingerprint + u")."_s;
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
    noteClaim(asked, still->name);
    if (name == u"hello"_s)
        return greet(asked);
    if (name == u"computers"_s)
        return computers();
    if (name == u"forget"_s)
        return forget(asked.value("name"_L1).toString(), *still);
    if (name == u"unlink"_s) {
        const auto gone = m_computers.unlink(m_fingerprint);
        if (!gone)
            return HubRequests::refusal(gone.error());
        qCInfo(lcHub).noquote() << gone->name << "unlinked itself: its key is gone.";
        return ending(succeeded({{"computer", gone->name}}));
    }
    if (!relayed().contains(name)) {
        qCWarning(lcHub).noquote() << still->name << "asked for" << (name.isEmpty() ? u"no request"_s : u"the request "_s + shown(name))
                                   << "which the hub doesn't take: refused.";
        return HubRequests::refusal(name.isEmpty() ? Failure(Failure::Code::HubBadRequest)
                                                   : Failure(Failure::Code::HubUnknownRequest, {name}));
    }
    // Who asks is the key's computer, never what the line says.
    QJsonObject handed = asked;
    handed.insert("computer"_L1, still->name);
    const QByteArray answered = m_relay(compact(handed));
    const QJsonDocument document = QJsonDocument::fromJson(answered);
    return document.isObject() ? document.object() : HubRequests::refusal(Failure(Failure::Code::HubBadRequest));
}

QJsonObject HubSession::computers() const {
    QJsonArray computers;
    for (const HubComputers::Computer &computer : m_computers.computers())
        computers.append(QJsonObject{{"name", computer.name},
                                     {"fingerprint", computer.fingerprint},
                                     {"linked", computer.linked.toString(Qt::ISODate)},
                                     {"this", computer.fingerprint == m_fingerprint}});
    return succeeded({{"computers", computers}});
}

QJsonObject HubSession::forget(const QString &named, const HubComputers::Computer &asking) {
    // By its name alone: a fingerprint is no name, and a computer is forgotten as the person sees it listed.
    if (!HubComputers::isName(named))
        return HubRequests::refusal(Failure(Failure::Code::HubBadComputerName, {shown(named)}));
    const auto gone = m_computers.unlink(named);
    if (!gone)
        return HubRequests::refusal(gone.error());
    qCInfo(lcHub).noquote() << asking.name << "took the key of" << gone->name << u"("_s + gone->fingerprint + u") away."_s;
    // Its own key gone, nothing more is answered: as unlink.
    const QJsonObject answer = succeeded({{"computer", gone->name}});
    return gone->fingerprint == m_fingerprint ? ending(answer) : answer;
}

void HubSession::noteClaim(const QJsonObject &asked, const QString &name) const {
    if (const QString claimed = asked.value("computer"_L1).toString(); asked.contains("computer"_L1) && claimed != name)
        qCWarning(lcHub).noquote() << "The key of" << name << u"("_s + m_fingerprint + u") said it was"_s << shown(claimed)
                                   << "in its request: it is answered as" << name + u"."_s;
}
