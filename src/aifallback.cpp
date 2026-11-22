#include "aifallback.h"

#include <QJsonObject>
#include <QUrl>

#include "recognition.h"
#include "secretstore.h"
#include "serverreach.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString trimmedUrl(QString url) {
    url = url.trimmed();
    while (url.endsWith(u'/'))
        url.chop(1);
    return url;
}

[[nodiscard]] QJsonObject keptOf(const Settings &settings, Capability capability) {
    return AiPrivacy::chosen(settings, capability).value("fallback"_L1).toObject();
}

[[nodiscard]] AiServer serverAt(const QString &url, const QString &name) {
    return AiServer{.preset = u"custom"_s,
                    .name = name,
                    .baseUrl = url,
                    .key = AiEndpoints::keptKey(url, Secrets::shared()),
                    .keyFile = {},
                    .model = {}};
}

// Whether this capability's fallback to this computer is a server there
// (Ollama), rather than this computer's own ways of speech and voices.
[[nodiscard]] bool servedHere(Capability capability) {
    return capability == Capability::Answers || capability == Capability::Handwriting || capability == Capability::Embeddings;
}

[[nodiscard]] QString ruleName(AiFallback::Rule rule) {
    switch (rule) {
    case AiFallback::Rule::FallsBack:
        return u"falls-back"_s;
    case AiFallback::Rule::Waits:
        return u"waits"_s;
    case AiFallback::Rule::IfChosen:
        break;
    }
    return u"if-chosen"_s;
}

// Where the fallback is, and whether it may stand in for the first at all:
// none for this computer standing in for itself, a server that is the first,
// or someone else's computer without its own allowance (or for one who kept
// to their own).
[[nodiscard]] std::optional<AiFallback::Instead> placed(const Settings &settings, Capability capability, const AiFallback::Chosen &fallback,
                                                        const QString &first, const QString &firstModel, AiPrivacy::HostedAsked asked) {
    using AiFallback::To;
    AiFallback::Instead used{.to = fallback.to, .server = {}, .model = {}, .hosted = false, .first = QUrl(first).host()};
    // Vectors of two models can't be searched together: the first's, on either.
    used.model = capability == Capability::Embeddings || fallback.model.isEmpty() ? firstModel : fallback.model;
    if (fallback.to == To::ThisComputer) {
        if (Recognition::reach(first, settings.ownAddresses()) == Recognition::Reach::ThisComputer)
            return std::nullopt;
        if (servedHere(capability))
            used.server = serverAt(QString::fromLatin1(AiFallback::thisComputersOllama), QObject::tr("this computer"));
        return used;
    }
    if (fallback.url.isEmpty() || fallback.url == first)
        return std::nullopt;
    used.server = serverAt(fallback.url, QUrl(fallback.url).host());
    used.hosted = AiPrivacy::hosted(settings, fallback.url, used.model);
    if (used.hosted && (!fallback.hosted || !asked.value_or(true)))
        return std::nullopt;
    return used;
}

} // namespace

AiFallback::Rule AiFallback::rule(Capability capability) {
    switch (capability) {
    case Capability::Quick:
        return Rule::Waits;
    case Capability::Answers:
        return Rule::IfChosen;
    case Capability::Handwriting:
    case Capability::SpeechToText:
    case Capability::TextToSpeech:
    case Capability::Embeddings:
        break;
    }
    return Rule::FallsBack;
}

QString AiFallback::toName(To to) {
    switch (to) {
    case To::ThisComputer:
        return u"this-computer"_s;
    case To::Server:
        return u"server"_s;
    case To::None:
        break;
    }
    return u"none"_s;
}

std::optional<AiFallback::To> AiFallback::toNamed(const QString &name) {
    for (const To to : {To::None, To::ThisComputer, To::Server})
        if (toName(to) == name)
            return to;
    return std::nullopt;
}

AiFallback::Chosen AiFallback::chosen(const Settings &settings, Capability capability) {
    const Rule each = rule(capability);
    if (each == Rule::Waits)
        return {};
    const QJsonObject kept = keptOf(settings, capability);
    const To byDefault = each == Rule::FallsBack ? To::ThisComputer : To::None;
    return {.to = toNamed(kept.value("to"_L1).toString()).value_or(byDefault),
            .url = trimmedUrl(kept.value("url"_L1).toString()),
            .model = kept.value("model"_L1).toString().trimmed(),
            .hosted = kept.value("hosted"_L1).toBool(false)};
}

std::optional<AiFallback::Instead> AiFallback::instead(const Settings &settings, Capability capability, const QString &firstUrl,
                                                       const QString &firstModel, AiPrivacy::HostedAsked asked) {
    const Chosen fallback = chosen(settings, capability);
    if (settings.aiOff() || fallback.to == To::None || firstUrl.trimmed().isEmpty())
        return std::nullopt;
    const QString first = trimmedUrl(firstUrl);
    auto used = placed(settings, capability, fallback, first, firstModel, asked);
    if (!used)
        return std::nullopt;
    const bool trying = keptOf(settings, capability).value("trying"_L1).toBool(false);
    if (!trying && ServerReach::reachable(first))
        return std::nullopt;
    return used;
}

QVariantMap AiFallback::describe(const Settings &settings, Capability capability) {
    const Chosen fallback = chosen(settings, capability);
    const bool keyHeld = !fallback.url.isEmpty() && !AiEndpoints::keptKey(fallback.url, Secrets::shared()).isEmpty();
    return {{u"to"_s, toName(fallback.to)},
            {u"url"_s, fallback.url},
            {u"model"_s, fallback.model},
            {u"hosted"_s, fallback.hosted},
            {u"rule"_s, ruleName(rule(capability))},
            {u"chosenTo"_s, keptOf(settings, capability).contains("to"_L1)},
            {u"hasKey"_s, keyHeld},
            {u"inTheClear"_s, !fallback.url.isEmpty() && AiEndpoints::inTheClear(fallback.url)}};
}

void AiFallback::choose(Settings &settings, Capability capability, const QVariantMap &fields) {
    if (rule(capability) == Rule::Waits)
        return;
    if (!settings.hasCapabilities())
        AiPrivacy::carryOver(settings);
    QJsonObject all = settings.capabilities();
    QJsonObject one = all.value(Capabilities::name(capability)).toObject();
    QJsonObject fallback = one.value("fallback"_L1).toObject();
    if (const auto to = toNamed(fields.value(u"to"_s).toString()))
        fallback.insert(u"to"_s, toName(*to));
    for (const auto field : {"url"_L1, "model"_L1}) {
        if (!fields.contains(field))
            continue;
        const QString value = field == "url"_L1 ? trimmedUrl(fields.value(field).toString()) : fields.value(field).toString().trimmed();
        if (value.isEmpty())
            fallback.remove(field);
        else
            fallback.insert(field, value);
    }
    if (fields.contains(u"hosted"_s))
        fallback.insert(u"hosted"_s, fields.value(u"hosted"_s).toBool());
    one.insert(u"fallback"_s, fallback);
    all.insert(Capabilities::name(capability), one);
    settings.setCapabilities(all);
}

void AiFallback::tryInstead(Settings &settings, Capability capability) {
    if (!settings.hasCapabilities())
        AiPrivacy::carryOver(settings);
    QJsonObject all = settings.capabilities();
    QJsonObject one = all.value(Capabilities::name(capability)).toObject();
    QJsonObject fallback = one.value("fallback"_L1).toObject();
    fallback.insert(u"trying"_s, true);
    one.insert(u"fallback"_s, fallback);
    all.insert(Capabilities::name(capability), one);
    settings.setCapabilities(all);
}
