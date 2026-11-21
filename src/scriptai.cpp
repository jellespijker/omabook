#include "scriptai.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <expected>
#include <vector>

#include "aicall.h"
#include "failure.h"
#include "inkcodec.h"
#include "recognitionqueue.h"
#include "recognitionroute.h"
#include "scriptplugin.h"

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

} // namespace

ScriptAi::ScriptAi(ScriptPlugin *plugin) : m_plugin(plugin) {}

ScriptAi::~ScriptAi() = default;

QJSValue ScriptAi::build(QJSEngine &engine) {
    QFile bundled(u":/scriptassist.js"_s);
    if (!bundled.open(QIODevice::ReadOnly))
        return engine.newErrorObject(QJSValue::GenericError, u"Omabook's own side of omabook.ai is missing."_s);
    QJSEngine::setObjectOwnership(this, QJSEngine::CppOwnership);
    return engine.evaluate(QString::fromUtf8(bundled.readAll()), QString::fromLatin1(assistName)).call({engine.newQObject(this)});
}

bool ScriptAi::available() { return m_plugin->m_services.ai && m_plugin->m_services.ai(nullptr) != nullptr; }

void ScriptAi::ask(const QString &prompt, const QString &system, const QString &model, const QJSValue &onText, const QJSValue &resolve,
                   const QJSValue &reject) {
    QString why = u"No AI is set up."_s;
    std::unique_ptr<AiProvider> provider = m_plugin->m_services.ai ? m_plugin->m_services.ai(&why) : nullptr;
    if (!provider) {
        reject.call({why});
        return;
    }
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
    const auto unavailable = [](const QString &why, const QString &message) {
        return QVariantMap{{u"available"_s, false}, {u"why"_s, why}, {u"message"_s, message}};
    };
    const auto named = Recognition::kindNamed(kind);
    if (!named)
        return unavailable(u"unknown-kind"_s, tr("Only ink and audio are recognised, not %1.").arg(kind));
    if (m_plugin->m_services.settings == nullptr)
        return unavailable(u"no-settings"_s, tr("Nothing can be recognised here: Omabook's settings aren't open."));
    if (const auto refused = RecognitionRoute::refusal(*m_plugin->m_services.settings, *named, hosted))
        return unavailable(RecognitionRoute::lackName(refused->lack), refused->message);
    if (const auto lacking = m_queue ? m_queue->lacking(*named, hosted) : std::nullopt)
        return unavailable(RecognitionRoute::lackName(RecognitionRoute::Lack::NoModel), *lacking);
    return {{u"available"_s, true}, {u"why"_s, QString()}, {u"message"_s, QString()}};
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
