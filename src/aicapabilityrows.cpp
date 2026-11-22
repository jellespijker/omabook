#include "aicapabilityrows.h"

#include <QJsonObject>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "ai.h"
#include "aiendpoint.h"
#include "aiprivacy.h"
#include "airegistry.h"
#include "embeddings.h"
#include "gemini.h"
#include "recognizers.h"
#include "secretstore.h"
#include "servervoice.h"
#include "settings.h"
#include "voices.h"

using namespace Qt::StringLiterals;

namespace {

// Where each capability's provider is set up, in the older keys, and the
// fields of it the page writes: the model and the server's address.
struct SetUp {
    QLatin1StringView key;     // "recognition", "read_aloud", "meaning"; empty for answers and quick
    QLatin1StringView model;   // the field of the model; empty where the AI page has its own picker
    QLatin1StringView address; // the field of the server's address; empty for the AI server only
};

[[nodiscard]] SetUp setUpOf(Capability capability) {
    switch (capability) {
    case Capability::Handwriting:
        return {"recognition"_L1, "ink_model"_L1, {}};
    case Capability::SpeechToText:
        return {"recognition"_L1, "transcription_model"_L1, "transcription_url"_L1};
    case Capability::TextToSpeech:
        return {"read_aloud"_L1, "model"_L1, "url"_L1};
    case Capability::Embeddings:
        return {"meaning"_L1, "model"_L1, "url"_L1};
    case Capability::Answers:
    case Capability::Quick:
        break;
    }
    return {};
}

[[nodiscard]] QJsonObject olderOf(const Settings &settings, Capability capability) {
    switch (capability) {
    case Capability::Handwriting:
    case Capability::SpeechToText:
        return settings.recognition();
    case Capability::TextToSpeech:
        return settings.readAloud();
    case Capability::Embeddings:
        return settings.meaning();
    case Capability::Answers:
    case Capability::Quick:
        break;
    }
    return {};
}

// The agents the page offers for answers, as the settings dialog did; one
// chosen another way (codex, grok, pi) is offered too while it is chosen.
constexpr auto answering =
    std::to_array<QLatin1StringView>({"omarchy-default"_L1, "claude"_L1, "agy"_L1, "anthropic"_L1, "openai"_L1, "command"_L1, "gemini"_L1});

// The providers a capability may choose from, by id; the window words them.
[[nodiscard]] QStringList offered(Capability capability, const QString &chosen) {
    switch (capability) {
    case Capability::Answers: {
        QStringList ids;
        for (const QLatin1StringView id : answering)
            ids.append(id);
        if (!chosen.isEmpty() && !ids.contains(chosen))
            ids.append(chosen);
        return ids;
    }
    case Capability::Quick:
        return {u"answers"_s}; // the same provider as answers, for now (ADR 0008, as built in #143)
    case Capability::Handwriting:
        return {u"ai-server"_s, u"gemini"_s};
    case Capability::SpeechToText:
        // The first there is (voxtype, whisper.cpp, a server: #146), or one alone.
        return {u"auto"_s, u"voxtype"_s, u"whisper.cpp"_s, u"server"_s, u"gemini"_s};
    case Capability::TextToSpeech:
        return {u"auto"_s, u"piper"_s, u"speech-dispatcher"_s, u"server"_s, u"openai"_s, u"gemini"_s};
    case Capability::Embeddings:
        return {u"server"_s, u"gemini"_s};
    }
    return {};
}

// The provider the person chose for it, by the id `offered` gives it.
[[nodiscard]] QString chosenOf(const Settings &settings, Capability capability) {
    switch (capability) {
    case Capability::Answers: {
        // With the AI off, the provider it comes back to.
        const QString provider = settings.aiOff() ? AiPrivacy::chosen(settings, Capability::Answers).value("provider"_L1).toString()
                                                  : Capabilities::providerFor(settings.aiProvider());
        return provider == "local"_L1 || provider.isEmpty() ? u"openai"_s : provider;
    }
    case Capability::Quick:
        return u"answers"_s;
    case Capability::Handwriting:
        return AiPrivacy::chosen(settings, capability).value("provider"_L1).toString() == "gemini"_L1 ? u"gemini"_s : u"ai-server"_s;
    case Capability::SpeechToText: {
        const QString provider = AiPrivacy::chosen(settings, capability).value("provider"_L1).toString();
        return provider.isEmpty() ? u"auto"_s : provider;
    }
    case Capability::TextToSpeech: {
        const QString engine = settings.readAloud().value("engine"_L1).toString();
        return engine.isEmpty() ? u"auto"_s : engine;
    }
    case Capability::Embeddings:
        return AiPrivacy::chosen(settings, capability).value("provider"_L1).toString() == "gemini"_L1 ? u"gemini"_s : u"server"_s;
    }
    return {};
}

// OpenAI's voices (#147) are text to speech's "openai", not answers' server.
[[nodiscard]] bool openAiVoice(Capability capability, const QString &chosen) {
    return capability == Capability::TextToSpeech && chosen == "openai"_L1;
}

// The field of the older key a capability's model is kept in, for the provider chosen.
[[nodiscard]] QLatin1StringView modelField(Capability capability, const QString &chosen) {
    return openAiVoice(capability, chosen) ? "openai_model"_L1 : setUpOf(capability).model;
}

// What text to speech's row has besides: the voices of the engine in use,
// the one chosen and how fast, and whether it takes a pace at all.
void addVoice(QVariantMap &row, const Settings &settings) {
    const Speech speech = Voices::speech(settings, {});
    row.insert(u"voices"_s, Voices::offered(settings));
    row.insert(u"voice"_s, speech.voice);
    row.insert(u"rate"_s, speech.rate);
    row.insert(u"paced"_s, Voices::paced(settings));
}

// Whether a key is kept for the provider chosen: Gemini's one key, OpenAI's
// for its voices, or one for the address given here.
[[nodiscard]] bool keyHeld(const Settings &settings, Capability capability, const QString &chosen, const QString &address) {
    if (chosen == "gemini"_L1)
        return Gemini::hasKey();
    if (openAiVoice(capability, chosen))
        return Voices::openAiHasKey(settings);
    return !address.isEmpty() && !AiEndpoints::keptKey(address, Secrets::shared()).isEmpty();
}

[[nodiscard]] QString modelDefaultOf(const Settings &settings, Capability capability, const QString &chosen) {
    if (openAiVoice(capability, chosen))
        return QString::fromLatin1(ServerVoice::openAiModel);
    if (chosen == "gemini"_L1 && !setUpOf(capability).model.isEmpty())
        return Gemini::modelDefault(capability);
    switch (capability) {
    case Capability::Handwriting:
        return Ai::models(settings, AiEndpoints::modelsKey(settings)).main;
    case Capability::SpeechToText:
        return QString::fromLatin1(Recognizers::defaultTranscriptionModel);
    case Capability::TextToSpeech:
        return QString::fromLatin1(ServerVoice::defaultModel);
    case Capability::Embeddings:
        return QString::fromLatin1(Embeddings::defaultModel);
    case Capability::Answers:
    case Capability::Quick:
        break;
    }
    return {};
}

} // namespace

AiCapabilityRows::AiCapabilityRows(Settings *settings, QString defaultsFile, QObject *parent)
    : QObject(parent), m_settings(settings), m_defaultsFile(std::move(defaultsFile)), m_preview(settings) {
    connect(m_settings, &Settings::changed, this, [this] {
        if (m_watching)
            refresh();
    });
}

void AiCapabilityRows::watch(bool on) {
    m_watching = on;
    if (on)
        refresh();
    else
        m_preview.stop(); // the page is put away: so is what it plays
}

bool AiCapabilityRows::aiOn() const { return !m_settings->aiOff(); }

QVariantMap AiCapabilityRows::row(Capability capability) const {
    const CapabilityStatus status = AiRegistry::status(*m_settings, capability, {}, m_defaultsFile);
    QVariantMap row = status.toJson().toVariantMap();
    const QString chosen = chosenOf(*m_settings, capability);
    const SetUp setUp = setUpOf(capability);
    const QJsonObject older = olderOf(*m_settings, capability);
    const QString address = setUp.address.isEmpty() ? QString() : older.value(setUp.address).toString().trimmed();
    row.insert(u"hostedAllowed"_s, AiPrivacy::hostedAllowed(*m_settings, capability));
    row.insert(u"chosen"_s, chosen);
    row.insert(u"offered"_s, offered(capability, chosen));
    row.insert(u"address"_s, address);
    // Where it goes when no address is given: the AI server, where there is one.
    row.insert(u"addressDefault"_s, m_settings->aiProvider() == "openai"_L1 ? AiEndpoints::name(*m_settings) : QString());
    // Asked of the keyring only for an address given here: the AI server's
    // key is the AI's, set up with it.
    row.insert(u"hasKey"_s, keyHeld(*m_settings, capability, chosen, address));
    row.insert(u"inTheClear"_s, !address.isEmpty() && AiEndpoints::inTheClear(address));
    const QLatin1StringView model = modelField(capability, chosen);
    QString modelChoice = model.isEmpty() ? QString() : older.value(model).toString().trimmed();
    // Gemini's models are its own, kept with it.
    if (chosen == "gemini"_L1 && !setUp.model.isEmpty())
        modelChoice = Gemini::modelChosen(*m_settings, capability);
    row.insert(u"modelChoice"_s, modelChoice);
    row.insert(u"modelDefault"_s, modelDefaultOf(*m_settings, capability, chosen));
    // Whether what writes out speech gives each word its moment (#146): voxtype doesn't.
    if (capability == Capability::SpeechToText && status.available) {
        const auto ways = AiRegistry::recognizers(*m_settings, Recognition::Kind::Audio, true);
        row.insert(u"timed"_s, ways && !ways->empty() && ways->front()->timesWords());
    }
    if (capability == Capability::TextToSpeech && status.available)
        addVoice(row, *m_settings);
    return row;
}

void AiCapabilityRows::refresh() {
    if (!m_watching)
        return;
    QVariantList rows;
    for (const Capability capability : Capabilities::all)
        rows.append(row(capability));
    if (rows == m_rows)
        return;
    m_rows = rows;
    emit changed();
}

void AiCapabilityRows::carryOver() {
    if (!m_settings->hasCapabilities())
        AiPrivacy::carryOver(*m_settings);
}

void AiCapabilityRows::allowHosted(const QString &capability, bool allowed) {
    const auto which = Capabilities::named(capability);
    if (!which)
        return;
    carryOver();
    QJsonObject all = m_settings->capabilities();
    QJsonObject one = all.value(capability).toObject();
    one.insert(u"hosted"_s, allowed);
    all.insert(capability, one);
    const auto allows = [&all](Capability each) { return all.value(Capabilities::name(each)).toObject().value("hosted"_L1).toBool(false); };
    // The older keys an older Omabook reads say hosted only where every
    // capability they stand for allows it: never more than was allowed.
    const auto older = [](QJsonObject chosen, bool on) {
        chosen.insert(u"hosted"_s, on);
        return chosen;
    };
    m_settings->setRecognition(older(m_settings->recognition(), allows(Capability::Handwriting) && allows(Capability::SpeechToText)));
    m_settings->setMeaning(older(m_settings->meaning(), allows(Capability::Embeddings)));
    m_settings->setReadAloud(older(m_settings->readAloud(), allows(Capability::TextToSpeech)));
    // Last, so what the older keys followed doesn't stand.
    m_settings->setCapabilities(all);
    refresh();
}

void AiCapabilityRows::choose(const QString &capability, const QString &provider) {
    const auto which = Capabilities::named(capability);
    if (!which || !offered(*which, chosenOf(*m_settings, *which)).contains(provider))
        return;
    carryOver();
    switch (*which) {
    case Capability::Answers:
        // Which may be hosted is a choice of its own, made in its row.
        if (!m_settings->aiOff())
            m_settings->chooseAiProvider(Capabilities::aiProviderFor(provider));
        break;
    case Capability::Handwriting:
    case Capability::Embeddings:
    case Capability::SpeechToText: {
        QJsonObject all = m_settings->capabilities();
        QJsonObject one = all.value(capability).toObject();
        // The way it always went is no provider of its own.
        if (provider == "auto"_L1 || provider == "ai-server"_L1 || (provider == "server"_L1 && *which == Capability::Embeddings))
            one.remove(u"provider"_s);
        else
            one.insert(u"provider"_s, provider);
        all.insert(capability, one);
        m_settings->setCapabilities(all);
        break;
    }
    case Capability::TextToSpeech: {
        QJsonObject older = m_settings->readAloud();
        if (provider == "auto"_L1)
            older.remove(u"engine"_s);
        else
            older.insert(u"engine"_s, provider);
        m_settings->setReadAloud(older);
        break;
    }
    case Capability::Quick:
        break; // answers' provider, for now
    }
    refresh();
}

void AiCapabilityRows::setUp(Capability capability, const QString &field, const QString &value) {
    QJsonObject older = olderOf(*m_settings, capability);
    if (value.trimmed().isEmpty())
        older.remove(field);
    else
        older.insert(field, value.trimmed());
    if (capability == Capability::TextToSpeech)
        m_settings->setReadAloud(older);
    else if (capability == Capability::Embeddings)
        m_settings->setMeaning(older);
    else
        m_settings->setRecognition(older);
    refresh();
}

void AiCapabilityRows::setModel(const QString &capability, const QString &model) {
    const auto which = Capabilities::named(capability);
    if (!which || setUpOf(*which).model.isEmpty())
        return;
    if (const QString chosen = chosenOf(*m_settings, *which); chosen != "gemini"_L1) {
        setUp(*which, modelField(*which, chosen), model);
        return;
    }
    Gemini::chooseModel(*m_settings, *which, model);
    refresh();
}

void AiCapabilityRows::setAddress(const QString &capability, const QString &address) {
    const auto which = Capabilities::named(capability);
    if (which && !setUpOf(*which).address.isEmpty())
        setUp(*which, setUpOf(*which).address, address);
}

QString AiCapabilityRows::setKey(const QString &capability, const QString &key) {
    const auto which = Capabilities::named(capability);
    // Gemini has one key, whichever capability it is given in.
    if (which && chosenOf(*m_settings, *which) == "gemini"_L1) {
        const auto kept = Gemini::keepKey(key, Secrets::shared());
        refresh();
        emit keyKept();
        return kept ? QString() : kept.error().text();
    }
    // OpenAI's key is the one it answers with too.
    if (which && openAiVoice(*which, chosenOf(*m_settings, *which))) {
        const auto kept = AiEndpoints::keepKey(Voices::openAiUrl(*m_settings), key, Secrets::shared());
        refresh();
        emit keyKept();
        return kept ? QString() : kept.error();
    }
    if (!which || setUpOf(*which).address.isEmpty())
        return {};
    const QString address = olderOf(*m_settings, *which).value(setUpOf(*which).address).toString().trimmed();
    if (address.isEmpty())
        return {};
    const auto kept = AiEndpoints::keepKey(address, key, Secrets::shared());
    refresh();
    return kept ? QString() : kept.error();
}

void AiCapabilityRows::setVoice(const QString &voice) {
    Voices::keep(*m_settings, u"voice"_s, voice);
    refresh();
}

void AiCapabilityRows::setRate(double rate) {
    Voices::keep(*m_settings, u"rate"_s, std::round(std::clamp(rate, 0.5, 2.0) * 10) / 10);
    refresh();
}

void AiCapabilityRows::setAiOn(bool on) {
    if (on == aiOn())
        return;
    if (!on) {
        m_settings->setAiProvider(u"off"_s);
    } else {
        // Back to the provider answers had, which `capabilities` kept.
        const QString provider = AiPrivacy::chosen(*m_settings, Capability::Answers).value("provider"_L1).toString();
        m_settings->chooseAiProvider(Capabilities::aiProviderFor(provider.isEmpty() ? u"local"_s : provider));
    }
    refresh();
}
