#include "recognizers.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <chrono>
#include <cmath>
#include <optional>
#include <utility>

#include "inkpicture.h"
#include "openaiprovider.h"
#include "thinkingfilter.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

using Recognition::Failure;
using Why = Recognition::Failure::Why;

// What the vision model is told: the writing as it stands, nothing about it.
constexpr auto inkPrompt = "The picture is handwriting. Write out exactly what it says, keeping its lines, in the language it "
                           "is written in. Answer with the text alone: no comments, no quotation marks, no description of the "
                           "picture. When nothing in it can be read, answer with nothing."_L1;

[[nodiscard]] QNetworkRequest httpRequest(const AiServer &server, const QString &path, std::chrono::milliseconds timeout) {
    QNetworkRequest http(QUrl(server.baseUrl + path));
    if (const QString key = server.currentKey(); !key.isEmpty())
        http.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    http.setTransferTimeout(timeout);
    return http;
}

[[nodiscard]] QString serverMessage(const QByteArray &body) {
    const QJsonValue error = QJsonDocument::fromJson(body).object().value("error"_L1);
    return error.isString() ? error.toString() : error.toObject().value("message"_L1).toString();
}

// A sentence that begins with where it went ("the AI server at ...") begins with a capital.
[[nodiscard]] QString sentence(QString text) {
    if (!text.isEmpty())
        text[0] = text.at(0).toUpper();
    return text;
}

// A failure that only means waiting: the server isn't there, or not ready.
[[nodiscard]] std::optional<Failure> awayFailure(int status, const QNetworkReply &reply, const QString &where) {
    if (status == 0)
        return Failure{.why = Why::Away, .message = sentence(QObject::tr("%1 can't be reached now (%2).").arg(where, reply.errorString()))};
    if (status == 429 || status == 502 || status == 503 || status == 504)
        return Failure{.why = Why::Away, .message = sentence(QObject::tr("%1 is busy or starting (%2).").arg(where).arg(status))};
    return std::nullopt;
}

// Why a request failed, in plain words, and what is done about it.
[[nodiscard]] Failure failureOf(QNetworkReply &reply, const QByteArray &body, const QString &where, const QString &model,
                                bool transcribing) {
    const int status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (auto away = awayFailure(status, reply, where))
        return *away;
    const QString said = serverMessage(body);
    if (status == 401 || status == 403)
        return {.why = Why::Refused, .message = sentence(QObject::tr("%1 didn't accept the key; put one in the settings.").arg(where))};
    // A server without the service answers that the address isn't there.
    const bool aboutTheModel = said.contains("model"_L1, Qt::CaseInsensitive);
    if (transcribing && !aboutTheModel && (status == 404 || status == 405 || status == 501))
        return {.why = Why::Unsupported, .message = sentence(QObject::tr("%1 doesn't transcribe speech.").arg(where))};
    if (status == 404 || said.contains("not found"_L1, Qt::CaseInsensitive))
        return {.why = Why::Refused, .message = sentence(QObject::tr("%1 has no model \"%2\".").arg(where, model))};
    if (!transcribing && (said.contains("image"_L1, Qt::CaseInsensitive) || said.contains("vision"_L1, Qt::CaseInsensitive)))
        return {.why = Why::Refused,
                .message = QObject::tr("The model %1 on %2 can't read pictures; choose one that can for handwriting.").arg(model, where)};
    if (!said.isEmpty())
        return {.why = Why::Refused, .message = sentence(QObject::tr("%1 answered: %2").arg(where, said))};
    return {.why = Why::Refused, .message = sentence(QObject::tr("%1 answered with an error (%2).").arg(where).arg(status))};
}

// The type a recording is sent as, by its file's name.
[[nodiscard]] QString audioType(const QString &file) {
    const QString suffix = QFileInfo(file).suffix().toLower();
    if (suffix == "opus"_L1 || suffix == "ogg"_L1 || suffix == "oga"_L1)
        return u"audio/ogg"_s;
    if (suffix == "wav"_L1)
        return u"audio/wav"_s;
    if (suffix == "mp3"_L1)
        return u"audio/mpeg"_s;
    if (suffix == "m4a"_L1 || suffix == "mp4"_L1)
        return u"audio/mp4"_s;
    if (suffix == "flac"_L1 || suffix == "webm"_L1)
        return u"audio/"_s + suffix;
    return u"application/octet-stream"_s;
}

[[nodiscard]] qint64 msOf(const QJsonValue &seconds) { return std::llround(seconds.toDouble() * 1000.0); }

void addField(QHttpMultiPart &form, const QByteArray &name, const QString &value) {
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"" + name + "\"");
    part.setBody(value.toUtf8());
    form.append(part);
}

} // namespace

namespace Recognizers {

Vision::Vision(AiServer server, QString model, QObject *parent)
    : Recognizer(parent), m_server(std::move(server)), m_model(std::move(model)) {}

Vision::~Vision() { cancel(); }

QString Vision::where() const { return tr("the AI server at %1").arg(m_server.host()); }

void Vision::resolve() {
    if (!m_model.isEmpty() || !m_server.model.isEmpty()) {
        if (m_model.isEmpty())
            m_model = m_server.model;
        Recognizer::resolve();
        return;
    }
    // No model named: the server is asked which it has.
    m_reply = m_network.get(httpRequest(m_server, u"/models"_s, 30s));
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = std::exchange(m_reply, nullptr);
        if (reply == nullptr)
            return;
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            emit failed(failureOf(*reply, body, where(), {}, false));
            return;
        }
        QStringList listed;
        for (const QJsonValue &model : QJsonDocument::fromJson(body).object().value("data"_L1).toArray())
            listed.append(model.toObject().value("id"_L1).toString());
        const auto model = OpenAiProvider::defaultModel(m_server, listed);
        if (!model) {
            emit failed({.why = Why::Refused, .message = model.error()});
            return;
        }
        m_model = *model;
        emit resolved();
    });
}

void Vision::recognize(const Recognition::Input &input) {
    cancel();
    const QByteArray png = Ink::png(input.strokes);
    if (png.isEmpty()) {
        emit failed({.why = Why::Refused, .message = tr("There is no ink to recognise.")});
        return;
    }
    m_language = input.language;
    QString prompt = inkPrompt;
    if (!input.language.isEmpty())
        prompt += u" It is most likely written in the language whose code is \"%1\"."_s.arg(input.language);
    const QJsonArray content{
        QJsonObject{{"type", "text"}, {"text", prompt}},
        QJsonObject{{"type", "image_url"},
                    {"image_url", QJsonObject{{"url", u"data:image/png;base64,"_s + QString::fromLatin1(png.toBase64())}}}},
    };
    const QJsonObject body{{"model", m_model},
                           {"stream", false},
                           {"temperature", 0},
                           {"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", content}}}}};
    QNetworkRequest http = httpRequest(m_server, u"/chat/completions"_s, 5min);
    http.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    m_reply = m_network.post(http, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        if (QNetworkReply *reply = std::exchange(m_reply, nullptr)) {
            reply->deleteLater();
            read(reply);
        }
    });
}

void Vision::read(QNetworkReply *reply) {
    const QByteArray body = reply->readAll();
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || reply->error() != QNetworkReply::NoError) {
        emit failed(failureOf(*reply, body, where(), m_model, false));
        return;
    }
    const QJsonObject answer = QJsonDocument::fromJson(body).object();
    const QJsonObject message = answer.value("choices"_L1).toArray().at(0).toObject().value("message"_L1).toObject();
    // A thinking model's working is never the text, inside the answer or beside it.
    ThinkingFilter thinking;
    QString text = thinking.take(message.value("content"_L1).toString());
    text = (text + thinking.finish()).trimmed();
    emit recognized({.kind = Recognition::Kind::Ink,
                     .text = text,
                     .segments = {{.text = text, .start = 0, .end = 0, .words = {}}},
                     .language = m_language,
                     .model = answer.value("model"_L1).toString(m_model),
                     .server = where(),
                     .at = QDateTime::currentMSecsSinceEpoch(),
                     .kept = false});
}

void Vision::cancel() {
    if (QNetworkReply *reply = std::exchange(m_reply, nullptr)) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

Transcription::Transcription(AiServer server, QString model, QObject *parent)
    : Recognizer(parent), m_server(std::move(server)),
      m_model(model.isEmpty() ? QString::fromLatin1(defaultTranscriptionModel) : std::move(model)) {}

Transcription::~Transcription() { cancel(); }

QString Transcription::where() const { return tr("the transcription server at %1").arg(m_server.host()); }

void Transcription::recognize(const Recognition::Input &input) {
    cancel();
    auto file = std::make_unique<QFile>(input.file);
    if (!file->open(QIODevice::ReadOnly)) {
        emit failed({.why = Why::Refused, .message = tr("The recording %1 can't be read.").arg(input.file)});
        return;
    }
    auto form = std::make_unique<QHttpMultiPart>(QHttpMultiPart::FormDataType);
    QHttpPart audio;
    audio.setHeader(QNetworkRequest::ContentTypeHeader, audioType(input.file));
    audio.setHeader(QNetworkRequest::ContentDispositionHeader,
                    u"form-data; name=\"file\"; filename=\"%1\""_s.arg(QFileInfo(input.file).fileName()));
    // Read as it is sent: a recording of hours is never all in memory.
    audio.setBodyDevice(file.get());
    file.release()->setParent(form.get());
    form->append(audio);
    addField(*form, "model", m_model);
    addField(*form, "response_format", u"verbose_json"_s);
    addField(*form, "timestamp_granularities[]", u"word"_s);
    addField(*form, "timestamp_granularities[]", u"segment"_s);
    if (!input.language.isEmpty())
        addField(*form, "language", input.language);
    // Hours of speech take a while to write out, and a server says nothing meanwhile.
    m_reply = m_network.post(httpRequest(m_server, u"/audio/transcriptions"_s, 60min), form.get());
    form.release()->setParent(m_reply);
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        if (QNetworkReply *reply = std::exchange(m_reply, nullptr)) {
            reply->deleteLater();
            read(reply);
        }
    });
}

void Transcription::read(QNetworkReply *reply) {
    const QByteArray body = reply->readAll();
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || reply->error() != QNetworkReply::NoError) {
        emit failed(failureOf(*reply, body, where(), m_model, true));
        return;
    }
    QJsonParseError error{};
    const QJsonDocument answer = QJsonDocument::fromJson(body, &error);
    Recognition::Result result = error.error == QJsonParseError::NoError && answer.isObject()
                                     ? transcript(answer.object())
                                     : transcript(QJsonObject{{"text", QString::fromUtf8(body).trimmed()}});
    result.model = m_model;
    result.server = where();
    emit recognized(result);
}

void Transcription::cancel() {
    if (QNetworkReply *reply = std::exchange(m_reply, nullptr)) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

Recognition::Result transcript(const QJsonObject &answer) {
    Recognition::Result result{.kind = Recognition::Kind::Audio,
                               .text = answer.value("text"_L1).toString().trimmed(),
                               .segments = {},
                               .language = answer.value("language"_L1).toString(),
                               .model = {},
                               .server = {},
                               .at = QDateTime::currentMSecsSinceEpoch(),
                               .kept = false};
    for (const QJsonValue &value : answer.value("segments"_L1).toArray()) {
        const QJsonObject segment = value.toObject();
        result.segments.append({.text = segment.value("text"_L1).toString().trimmed(),
                                .start = msOf(segment.value("start"_L1)),
                                .end = msOf(segment.value("end"_L1)),
                                .words = {}});
    }
    const QJsonArray words = answer.value("words"_L1).toArray();
    if (result.segments.isEmpty() && (!result.text.isEmpty() || !words.isEmpty()))
        result.segments.append({.text = result.text,
                                .start = words.isEmpty() ? 0 : msOf(words.first().toObject().value("start"_L1)),
                                .end = words.isEmpty() ? 0 : msOf(words.last().toObject().value("end"_L1)),
                                .words = {}});
    // Each word in the last segment that began at or before it.
    qsizetype at = 0;
    for (const QJsonValue &value : words) {
        const QJsonObject word = value.toObject();
        const Recognition::Word said{
            .text = word.value("word"_L1).toString().trimmed(), .start = msOf(word.value("start"_L1)), .end = msOf(word.value("end"_L1))};
        while (at + 1 < result.segments.size() && result.segments.at(at + 1).start <= said.start)
            ++at;
        result.segments[at].words.append(said);
    }
    return result;
}

} // namespace Recognizers
