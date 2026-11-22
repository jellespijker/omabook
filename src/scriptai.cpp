#include "scriptai.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtConcurrent/QtConcurrentRun>
#include <expected>
#include <tuple>
#include <vector>

#include "aicall.h"
#include "airegistry.h"
#include "audiodevices.h"
#include "embeddings.h"
#include "failure.h"
#include "inkcodec.h"
#include "recognitionqueue.h"
#include "scriptplugin.h"
#include "settings.h"
#include "voice.h"
#include "voices.h"

using namespace Qt::StringLiterals;

namespace {

// Run under the prelude's name, so its frames are never taken for the
// script's when an error says where it happened (ScriptPlugin::describe).
constexpr auto assistName = "omabook-api/assist";

[[nodiscard]] QString compact(const QJsonObject &object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }

// A stroke a script wrote out itself: {points: [{x, y, pressure, ms}], width}.
[[nodiscard]] Ink::Stroke drawn(const QJsonObject &given, qsizetype index) {
    Ink::Stroke stroke{.id = QString::number(index),
                       .color = Qt::black,
                       .width = given.value("width"_L1).toDouble(2.0),
                       .pressured = false,
                       .tilted = false,
                       .highlighter = false,
                       .began = given.value("began"_L1).toInteger(index),
                       .points = {}};
    for (const auto &value : given.value("points"_L1).toArray()) {
        const QJsonObject point = value.toObject();
        stroke.pressured = stroke.pressured || point.contains("pressure"_L1);
        stroke.points.push_back({.at = {point.value("x"_L1).toDouble(), point.value("y"_L1).toDouble()},
                                 .pressure = static_cast<float>(point.value("pressure"_L1).toDouble(0.5)),
                                 .xTilt = 0.0F,
                                 .yTilt = 0.0F,
                                 .ms = static_cast<quint32>(point.value("ms"_L1).toInteger())});
    }
    return stroke;
}

// The strokes a script hands over: the items of a set of strokes, as
// `omabook.log` keeps them ({id: base64}), or a list of strokes, each its
// base64 (as an InkSurface's batch holds it) or its points.
[[nodiscard]] std::expected<std::vector<Ink::Stroke>, QString> strokesOf(const QString &json) {
    const QJsonDocument given = QJsonDocument::fromJson(json.toUtf8());
    if (given.isObject()) {
        Ink::Read read = Ink::fromItems(given.object());
        if (read.strokes.empty() && !read.problems.isEmpty())
            return std::unexpected(read.problems.first());
        return std::move(read.strokes);
    }
    std::vector<Ink::Stroke> strokes;
    const QJsonArray list = given.array();
    for (qsizetype index = 0; index < list.size(); ++index) {
        const QJsonValue stroke = list.at(index);
        if (stroke.isObject()) {
            strokes.push_back(drawn(stroke.toObject(), index));
            continue;
        }
        const auto decoded = Ink::decode(QByteArray::fromBase64(stroke.toString().toLatin1()), QString::number(index));
        if (!decoded)
            return std::unexpected(::Failure(::Failure::Code::StrokeUnreadable, {QString::number(index + 1), decoded.error()}).text());
        strokes.push_back(*decoded);
    }
    return strokes;
}

// The capability the options name: the one said, else quick for the model "quick", else answers.
[[nodiscard]] std::optional<Capability> capabilityOf(const QJsonObject &options) {
    if (const QString named = options.value("capability"_L1).toString(); !named.isEmpty())
        return Capabilities::named(named);
    return options.value("model"_L1).toString().trimmed() == "quick"_L1 ? Capability::Quick : Capability::Answers;
}

[[nodiscard]] bool answering(Capability capability) { return capability == Capability::Answers || capability == Capability::Quick; }

// What a plugin said of hosted providers, as the rule takes it (ADR 0008):
// for answers and quick, where it sends a prompt it wrote, saying nothing
// takes the person's choice; for the other four, where it sends the person's
// own material whole, only `hosted: true` asks for a hosted one.
[[nodiscard]] AiPrivacy::HostedAsked hostedAsked(Capability capability, const QJsonObject &options) {
    const QJsonValue said = options.value("hosted"_L1);
    if (answering(capability))
        return said.isBool() ? AiPrivacy::HostedAsked(said.toBool()) : std::nullopt;
    return said.toBool(false);
}

[[nodiscard]] QVariantMap unavailable(const QString &capability, const QString &why, const QString &message) {
    return {{u"capability"_s, capability}, {u"available"_s, false}, {u"hosted"_s, false}, {u"provider"_s, QString()},
            {u"model"_s, QString()},       {u"why"_s, why},          {u"message"_s, message}};
}

} // namespace

ScriptAi::ScriptAi(ScriptPlugin *plugin) : m_plugin(plugin) {
    // The window's stop (the bar over the page, Shift+R) stops it as the plugin's own does.
    if (const AudioDevices *audio = m_plugin->m_services.audio)
        connect(audio, &AudioDevices::stopSpeakingAsked, this, &ScriptAi::stopSpeaking);
}

ScriptAi::~ScriptAi() {
    if (AudioDevices *audio = m_plugin->m_services.audio; audio && m_voice)
        audio->endedSpeaking(this);
}

QJSValue ScriptAi::build(QJSEngine &engine) {
    QFile bundled(u":/scriptassist.js"_s);
    if (!bundled.open(QIODevice::ReadOnly))
        return engine.newErrorObject(QJSValue::GenericError, u"Omabook's own side of omabook.ai is missing."_s);
    QJSEngine::setObjectOwnership(this, QJSEngine::CppOwnership);
    return engine.evaluate(QString::fromUtf8(bundled.readAll()), QString::fromLatin1(assistName)).call({engine.newQObject(this)});
}

bool ScriptAi::available() { return m_plugin->m_services.ai && m_plugin->m_services.ai(nullptr) != nullptr; }

QVariantMap ScriptAi::statusOf(Capability capability, const QJsonObject &options, std::unique_ptr<AiProvider> *provider) {
    const Settings *settings = m_plugin->m_services.settings;
    const QString named = Capabilities::name(capability);
    QVariantMap status;
    if (settings) {
        const AiRegistry::Ask ask{.model = options.value("model"_L1).toString(), .hosted = hostedAsked(capability, options)};
        status = AiRegistry::status(*settings, capability, ask).toJson().toVariantMap();
    } else if (!answering(capability)) {
        return unavailable(named, u"no-settings"_s, tr("Nothing can be asked here: Omabook's settings aren't open."));
    } else if (options.value("hosted"_L1).toBool(true)) {
        // Where it can't be told, it is taken for someone else's.
        status = {{u"capability"_s, named}, {u"available"_s, true}, {u"hosted"_s, true}, {u"provider"_s, QString()},
                  {u"model"_s, QString()},  {u"why"_s, QString()},  {u"message"_s, QString()}};
    } else {
        status = unavailable(named, Capabilities::lackName(CapabilityLack::HostedNotChosen),
                             tr("The AI you chose is someone else's computer, and this plugin asks only your own."));
    }
    if (!answering(capability))
        return status;
    // The privacy rule and AI Off are the registry's to say; whether there is
    // a model is the one the plugin is handed (the window's, a command's, a test's).
    const QString lack = status.value(u"why"_s).toString();
    if (lack == Capabilities::lackName(CapabilityLack::AiOff) || lack == Capabilities::lackName(CapabilityLack::HostedNotAllowed)
        || lack == Capabilities::lackName(CapabilityLack::HostedNotChosen))
        return status;
    QString why = tr("No AI is set up.");
    std::unique_ptr<AiProvider> made = m_plugin->m_services.ai ? m_plugin->m_services.ai(&why) : nullptr;
    if (!made)
        return unavailable(named, lack.isEmpty() ? Capabilities::lackName(CapabilityLack::NoServer) : lack, why);
    status.insert(u"available"_s, true);
    status.insert(u"why"_s, QString());
    status.insert(u"message"_s, QString());
    if (provider)
        *provider = std::move(made);
    return status;
}

QVariantMap ScriptAi::status(const QString &options) {
    const QJsonObject chosen = QJsonDocument::fromJson(options.toUtf8()).object();
    const auto capability = capabilityOf(chosen);
    if (!capability)
        return unavailable(chosen.value("capability"_L1).toString(), u"unknown-capability"_s,
                           tr("Omabook has no capability called %1.").arg(chosen.value("capability"_L1).toString()));
    return statusOf(*capability, chosen);
}

void ScriptAi::ask(const QString &prompt, const QString &options, const QJSValue &onText, const QJSValue &resolve, const QJSValue &reject) {
    const QJsonObject chosen = QJsonDocument::fromJson(options.toUtf8()).object();
    const auto capability = capabilityOf(chosen);
    if (!capability || !answering(*capability)) {
        reject.call({tr("ai.ask asks for answers or quick work; %1 is asked otherwise.").arg(chosen.value("capability"_L1).toString())});
        return;
    }
    std::unique_ptr<AiProvider> provider;
    if (const QVariantMap found = statusOf(*capability, chosen, &provider); !found.value(u"available"_s).toBool()) {
        reject.call({found.value(u"message"_s).toString()});
        return;
    }
    const QString system = chosen.value("system"_L1).toString();
    const QString word = chosen.value("model"_L1).toString();
    const QString model = word.isEmpty() && *capability == Capability::Quick ? u"quick"_s : word;
    auto *asking = new AiCall(std::move(provider), m_plugin);
    m_plugin->busy(1);
    const auto finish = [this, asking](const QJSValue &callback, const QString &text) {
        callback.call({text});
        asking->deleteLater();
        m_plugin->busy(-1);
    };
    connect(asking, &AiCall::done, m_plugin, [finish, resolve](const QString &answer) { finish(resolve, answer); });
    connect(asking, &AiCall::failed, m_plugin, [finish, reject](const Failure &problem) { finish(reject, problem.text()); });
    // The answer as it is written, for a script that shows it growing.
    if (onText.isCallable())
        connect(asking, &AiCall::delta, m_plugin, [this, onText](const QString &piece) {
            QString problem;
            m_plugin->call(onText, {piece}, &problem);
            if (!problem.isEmpty())
                m_plugin->report(problem);
        });
    AiRequest request;
    request.system = system;
    request.messages.append({u"user"_s, prompt});
    // What a plugin writes is for a person to read, so the main model
    // unless it says otherwise: "quick", or a model by name.
    if (!model.isEmpty())
        Ai::useModel(request, model);
    asking->ask(request);
}

bool ScriptAi::recognizes(const QString &kind, bool hosted) { return recognition(kind, hosted).value(u"available"_s).toBool(); }

QVariantMap ScriptAi::recognition(const QString &kind, bool hosted) {
    const auto named = Recognition::kindNamed(kind);
    if (!named)
        return unavailable({}, u"unknown-kind"_s, tr("Only ink and audio are recognised, not %1.").arg(kind));
    const Capability capability = *named == Recognition::Kind::Ink ? Capability::Handwriting : Capability::SpeechToText;
    if (m_plugin->m_services.settings == nullptr)
        return unavailable(Capabilities::name(capability), u"no-settings"_s,
                           tr("Nothing can be recognised here: Omabook's settings aren't open."));
    QVariantMap status = statusOf(capability, {{u"hosted"_s, hosted}});
    if (!status.value(u"available"_s).toBool())
        return status;
    if (const auto lacking = m_queue ? m_queue->lacking(*named, hosted) : std::nullopt)
        return unavailable(Capabilities::name(capability), Capabilities::lackName(CapabilityLack::NoModel), *lacking);
    return status;
}

int ScriptAi::ink(const QString &strokes, const QString &options, const QJSValue &onWaiting, const QJSValue &resolve,
                  const QJSValue &reject) {
    auto read = strokesOf(strokes);
    if (!read) {
        reject.call({read.error()});
        return 0;
    }
    return submit({.kind = Recognition::Kind::Ink, .strokes = std::move(*read), .file = {}, .language = {}},
                  QJsonDocument::fromJson(options.toUtf8()).object(), onWaiting, resolve, reject);
}

int ScriptAi::audio(const QString &file, const QString &options, const QJSValue &onWaiting, const QJSValue &resolve,
                    const QJSValue &reject) {
    const QString path = m_plugin->inside(file, false);
    if (path.isEmpty()) {
        reject.call({tr("The recording \"%1\" can't be read: it should be a file of the plugin's own folder.").arg(file)});
        return 0;
    }
    return submit({.kind = Recognition::Kind::Audio, .strokes = {}, .file = path, .language = {}},
                  QJsonDocument::fromJson(options.toUtf8()).object(), onWaiting, resolve, reject);
}

bool ScriptAi::cancel(int job) { return m_queue && m_queue->cancel(job); }

int ScriptAi::submit(Recognition::Input input, const QJsonObject &options, const QJSValue &onWaiting, const QJSValue &resolve,
                     const QJSValue &reject) {
    RecognitionQueue *const recognizing = queue();
    if (recognizing == nullptr) {
        reject.call({tr("Nothing can be recognised here: Omabook's settings aren't open.")});
        return 0;
    }
    input.language = options.value("language"_L1).toString().trimmed();
    m_plugin->busy(1);
    RecognitionQueue::Request request{.input = std::move(input),
                                      .owner = m_plugin->manifest().id,
                                      .hosted = options.value("hosted"_L1).toBool(),
                                      .again = options.value("again"_L1).toBool()};
    const auto done = [this, resolve, reject](const std::expected<Recognition::Result, QString> &outcome) {
        if (outcome)
            resolve.call({compact(outcome->toJson())});
        else
            reject.call({outcome.error()});
        m_plugin->busy(-1);
    };
    const auto waiting = [this, onWaiting](const QString &why) {
        if (!onWaiting.isCallable())
            return;
        QString problem;
        m_plugin->call(onWaiting, {why}, &problem);
        if (!problem.isEmpty())
            m_plugin->report(problem);
    };
    return recognizing->submit(std::move(request), done, waiting);
}

RecognitionQueue *ScriptAi::queue() {
    if (!m_queue && m_plugin->m_services.settings != nullptr)
        m_queue =
            std::make_unique<RecognitionQueue>(RecognitionQueue::routeFor(*m_plugin->m_services.settings), m_plugin->m_services.library);
    return m_queue.get();
}

void ScriptAi::speak(const QString &text, const QString &options, const QJSValue &resolve, const QJSValue &reject) {
    const Settings *settings = m_plugin->m_services.settings;
    if (settings == nullptr) {
        reject.call({tr("Nothing can be read aloud here: Omabook's settings aren't open.")});
        return;
    }
    const QJsonObject chosen = QJsonDocument::fromJson(options.toUtf8()).object();
    // A local voice reads with the AI Off too: it only reads the text (ADR 0008).
    auto voice = AiRegistry::voice(*settings, hostedAsked(Capability::TextToSpeech, chosen));
    if (!voice) {
        reject.call({voice.error().text()});
        return;
    }
    stopSpeaking();
    m_voice = std::move(*voice);
    m_spoken = resolve;
    m_notSpoken = reject;
    m_plugin->busy(1);
    connect(m_voice.get(), &Voice::spoken, this, [this] { settleSpeech({}); });
    connect(m_voice.get(), &Voice::failed, this, [this](const Failure &why) { settleSpeech(why.text()); });
    // Read where the person sees who reads, and can stop it.
    if (AudioDevices *audio = m_plugin->m_services.audio)
        audio->beganSpeaking(this, m_plugin->m_manifest.name.isEmpty() ? m_plugin->m_manifest.id : m_plugin->m_manifest.name);
    m_voice->say(text, Voices::speech(*settings, chosen.value("language"_L1).toString()));
}

void ScriptAi::stopSpeaking() {
    if (!m_voice)
        return;
    m_voice->stop();
    // Stopped is not a failure: the promise is kept, as for a sentence read.
    settleSpeech({});
}

void ScriptAi::settleSpeech(const QString &why) {
    if (!m_voice)
        return;
    // Its signal may still be on the way out of it: let it go once that is done.
    m_voice->disconnect(this);
    m_voice.release()->deleteLater();
    if (AudioDevices *audio = m_plugin->m_services.audio)
        audio->endedSpeaking(this);
    const QJSValue told = why.isEmpty() ? m_spoken : m_notSpoken;
    m_spoken = m_notSpoken = QJSValue();
    told.call(why.isEmpty() ? QJSValueList() : QJSValueList{why});
    m_plugin->busy(-1);
}

void ScriptAi::embed(const QString &texts, const QString &options, const QJSValue &resolve, const QJSValue &reject) {
    const Settings *settings = m_plugin->m_services.settings;
    if (settings == nullptr) {
        reject.call({tr("Nothing can be embedded here: Omabook's settings aren't open.")});
        return;
    }
    const QJsonObject chosen = QJsonDocument::fromJson(options.toUtf8()).object();
    const auto route = AiRegistry::embeddings(*settings, hostedAsked(Capability::Embeddings, chosen));
    if (!route) {
        reject.call({route.error().text()});
        return;
    }
    QStringList given;
    for (const auto &text : QJsonDocument::fromJson(texts.toUtf8()).array())
        given << text.toString();
    const auto use = chosen.value("use"_L1).toString() == "question"_L1 ? Embeddings::Use::Question : Embeddings::Use::Document;
    m_plugin->busy(1);
    // The continuation answers the script by itself, on the plugin's thread; nothing waits for it.
    std::ignore = QtConcurrent::run([route = *route, given, use] { return Embeddings::embed(route, given, use); })
        .then(m_plugin, [this, resolve, reject](const std::expected<Embeddings::Vectors, Failure> &made) {
            if (made) {
                QJsonArray vectors;
                for (const auto &vector : *made) {
                    QJsonArray numbers;
                    for (const float number : vector)
                        numbers.append(double{number});
                    vectors.append(numbers);
                }
                resolve.call({compact(QJsonObject{{u"vectors"_s, vectors}})});
            } else {
                reject.call({made.error().text()});
            }
            m_plugin->busy(-1);
        });
}
