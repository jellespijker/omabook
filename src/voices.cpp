#include "voices.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

#include "aiendpoint.h"
#include "localvoices.h"
#include "recognition.h"
#include "secretstore.h"
#include "servervoice.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString chosen(const Settings &settings, QLatin1StringView key) {
    return settings.readAloud().value(key).toString().trimmed();
}

[[nodiscard]] QString piperProgram(const Settings &settings) {
    const QString named = chosen(settings, "piper_program"_L1);
    return named.isEmpty() ? PiperVoice::findProgram() : QStandardPaths::findExecutable(named);
}

[[nodiscard]] QStringList ownServers(const Settings &settings) {
    QStringList own;
    for (const auto &address : settings.recognition().value("own"_L1).toArray())
        own << address.toString();
    return own;
}

[[nodiscard]] QString serverModel(const Settings &settings) {
    const QString model = chosen(settings, "model"_L1);
    return model.isEmpty() ? QString::fromLatin1(ServerVoice::defaultModel) : model;
}

[[nodiscard]] AiServer custom(const QString &base) {
    return {.preset = u"custom"_s,
            .name = QUrl(base).host(),
            .baseUrl = base,
            .key = AiEndpoints::keptKey(base, Secrets::shared()),
            .keyFile = {},
            .model = {}};
}

// The server chosen to read aloud, if it may be asked: the one named for it,
// else the AI server.
[[nodiscard]] std::expected<AiServer, Failure> server(const Settings &settings) {
    if (settings.aiProvider() == "off"_L1)
        return std::unexpected(Failure(Failure::Code::VoiceAiOff));
    QString base = chosen(settings, "url"_L1);
    while (base.endsWith(u'/'))
        base.chop(1);
    std::expected<AiServer, QString> found =
        base.isEmpty() ? AiEndpoints::server(settings) : std::expected<AiServer, QString>(custom(base));
    if (!found)
        return std::unexpected(Failure::said(found.error()));
    const QString model = serverModel(settings);
    const bool elsewhere = AiEndpoints::runsElsewhere(model);
    const bool hosted = elsewhere || Recognition::reach(found->baseUrl, ownServers(settings)) == Recognition::Reach::Hosted;
    if (hosted && !settings.readAloud().value("hosted"_L1).toBool(false))
        return std::unexpected(Failure(Failure::Code::VoiceServerHosted, {elsewhere ? model : found->host()}));
    return *found;
}

[[nodiscard]] bool piperReady(const Settings &settings) { return !piperProgram(settings).isEmpty() && !PiperVoice::findModels().isEmpty(); }
[[nodiscard]] bool dispatcherReady() {
    return !QStandardPaths::findExecutable(QString::fromLatin1(SpeechDispatcherVoice::program)).isEmpty();
}

} // namespace

std::expected<QString, Failure> Voices::engine(const Settings &settings) {
    const QString wanted = chosen(settings, "engine"_L1);
    if (wanted == "server"_L1) {
        if (auto reachable = server(settings); !reachable)
            return std::unexpected(reachable.error());
        return u"server"_s;
    }
    if (wanted == "piper"_L1) {
        if (piperProgram(settings).isEmpty())
            return std::unexpected(Failure(Failure::Code::PiperMissing));
        if (PiperVoice::findModels().isEmpty())
            return std::unexpected(Failure(Failure::Code::PiperNoVoice));
        return u"piper"_s;
    }
    if (wanted == "speech-dispatcher"_L1 || !piperReady(settings)) {
        if (!dispatcherReady())
            return std::unexpected(
                Failure(wanted.isEmpty() || wanted == "auto"_L1 ? Failure::Code::NoVoice : Failure::Code::DispatcherMissing));
        return u"speech-dispatcher"_s;
    }
    return u"piper"_s;
}

std::expected<std::unique_ptr<Voice>, Failure> Voices::choose(const Settings &settings) {
    const auto which = engine(settings);
    if (!which)
        return std::unexpected(which.error());
    if (*which == "piper"_L1)
        return std::make_unique<PiperVoice>(piperProgram(settings), PiperVoice::findModels());
    if (*which == "server"_L1) {
        auto reachable = server(settings);
        if (!reachable)
            return std::unexpected(reachable.error());
        return std::make_unique<ServerVoice>(*std::move(reachable), serverModel(settings));
    }
    return std::make_unique<SpeechDispatcherVoice>(QStandardPaths::findExecutable(QString::fromLatin1(SpeechDispatcherVoice::program)));
}

QStringList Voices::offered(const Settings &settings) {
    const auto which = engine(settings);
    if (!which)
        return {};
    if (*which == "piper"_L1) {
        QStringList names;
        std::ranges::transform(PiperVoice::findModels(), std::back_inserter(names),
                               [](const QString &file) { return QFileInfo(file).completeBaseName(); });
        return names;
    }
    return *which == "server"_L1 ? ServerVoice::voices() : SpeechDispatcherVoice::voices();
}

Speech Voices::speech(const Settings &settings, const QString &language) {
    const QJsonObject chosenNow = settings.readAloud();
    return {.rate = std::clamp(chosenNow.value("rate"_L1).toDouble(1.0), 0.5, 2.0),
            .voice = chosenNow.value("voice"_L1).toString(),
            .language = language};
}
