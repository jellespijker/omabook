#include "gemini.h"

#include <QDataStream>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include "ai.h"
#include "aifailures.h"
#include "secretstore.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

// Each JSON field as the REST API writes it.
[[nodiscard]] QJsonObject textPart(const QString &text) { return {{"text"_L1, text}}; }
[[nodiscard]] QJsonObject blobPart(const QString &mimeType, const QByteArray &bytes) {
    return {{"inlineData"_L1, QJsonObject{{"mimeType"_L1, mimeType}, {"data"_L1, QString::fromLatin1(bytes.toBase64())}}}};
}
[[nodiscard]] QJsonObject userTurn(const QJsonArray &parts) { return {{"role"_L1, u"user"_s}, {"parts"_L1, parts}}; }

[[nodiscard]] QJsonArray partsOf(const QJsonObject &response) {
    return response.value("candidates"_L1).toArray().at(0).toObject().value("content"_L1).toObject().value("parts"_L1).toArray();
}

// "1.250s", as a Duration is written, in milliseconds.
[[nodiscard]] qint64 msOf(const QJsonValue &duration) {
    QString seconds = duration.toString();
    if (seconds.endsWith(u's'))
        seconds.chop(1);
    return std::llround(seconds.toDouble() * 1000.0);
}

// A WAV around 16-bit mono PCM, which Gemini streams speech as (audio/L16;rate=24000).
[[nodiscard]] QByteArray wav(const QByteArray &pcm, quint32 rate) {
    QByteArray out;
    QDataStream stream(&out, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    const auto size = static_cast<quint32>(pcm.size());
    stream.writeRawData("RIFF", 4);
    stream << 36 + size;
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32{16} << quint16{1} << quint16{1} << rate << (rate * 2) << quint16{2} << quint16{16};
    stream.writeRawData("data", 4);
    stream << size;
    stream.writeRawData(pcm.constData(), pcm.size());
    return out;
}

[[nodiscard]] QString keptKey(const SecretStore &store) {
    const QString kept = store.get(QString::fromLatin1(Gemini::secretName)).trimmed();
    return kept.isEmpty() ? qEnvironmentVariable(Gemini::keyVariable).trimmed() : kept;
}

// A model may be named as its resource ("models/gemini-3.8-flash").
[[nodiscard]] QString bare(QString model) {
    model = model.trimmed();
    if (model.startsWith("models/"_L1))
        model.remove(0, 7);
    return model;
}

} // namespace

QString Gemini::Api::host() const { return QUrl(baseUrl).host(); }

std::expected<Gemini::Api, Failure> Gemini::api(const Settings &settings) {
    QString url = settings.gemini().value("url"_L1).toString().trimmed();
    while (url.endsWith(u'/'))
        url.chop(1);
    const QString key = keptKey(Secrets::shared());
    if (key.isEmpty())
        return std::unexpected(Failure(Failure::Code::GeminiNoKey));
    return Api{.baseUrl = url.isEmpty() ? QString::fromLatin1(defaultUrl) : url, .key = key};
}

bool Gemini::hasKey() { return !keptKey(Secrets::shared()).isEmpty(); }

std::expected<void, Failure> Gemini::keepKey(const QString &key, SecretStore &store) {
    const QString name = QString::fromLatin1(secretName);
    if (key.trimmed().isEmpty()) {
        if (!store.forget(name))
            return std::unexpected(Failure(Failure::Code::KeyNotForgotten));
        return {};
    }
    QString error;
    if (!store.put(name, key.trimmed(), &error))
        return std::unexpected(Failure(Failure::Code::KeyNotKept, {error}));
    return {};
}

QString Gemini::modelDefault(Capability capability) {
    switch (capability) {
    case Capability::Answers:
        return QString::fromLatin1(answersModel);
    case Capability::Quick:
        return QString::fromLatin1(quickModel);
    case Capability::Handwriting:
        return QString::fromLatin1(visionModel);
    case Capability::SpeechToText:
        return QString::fromLatin1(transcriptionModel);
    case Capability::TextToSpeech:
        return QString::fromLatin1(voiceModel);
    case Capability::Embeddings:
        return QString::fromLatin1(embeddingModel);
    }
    return {};
}

QString Gemini::modelChosen(const Settings &settings, Capability capability) {
    return bare(settings.gemini().value("models"_L1).toObject().value(Capabilities::name(capability)).toString());
}

QString Gemini::modelFor(const Settings &settings, Capability capability) {
    const QString chosen = modelChosen(settings, capability);
    return chosen.isEmpty() ? modelDefault(capability) : chosen;
}

void Gemini::chooseModel(Settings &settings, Capability capability, const QString &model) {
    QJsonObject gemini = settings.gemini();
    QJsonObject models = gemini.value("models"_L1).toObject();
    if (bare(model).isEmpty())
        models.remove(Capabilities::name(capability));
    else
        models.insert(Capabilities::name(capability), bare(model));
    gemini.insert(u"models"_s, models);
    settings.setGemini(gemini);
}

QStringList Gemini::voices() {
    return {u"Kore"_s,      u"Zephyr"_s,        u"Puck"_s,         u"Charon"_s,    u"Fenrir"_s,     u"Leda"_s,
            u"Orus"_s,      u"Aoede"_s,         u"Callirrhoe"_s,   u"Autonoe"_s,   u"Enceladus"_s,  u"Iapetus"_s,
            u"Umbriel"_s,   u"Algieba"_s,       u"Despina"_s,      u"Erinome"_s,   u"Algenib"_s,    u"Rasalgethi"_s,
            u"Laomedeia"_s, u"Achernar"_s,      u"Alnilam"_s,      u"Schedar"_s,   u"Gacrux"_s,     u"Pulcherrima"_s,
            u"Achird"_s,    u"Zubenelgenubi"_s, u"Vindemiatrix"_s, u"Sadachbia"_s, u"Sadaltager"_s, u"Sulafat"_s};
}

QNetworkRequest Gemini::request(const Api &api, const QString &model, QStringView method, std::chrono::milliseconds timeout) {
    QNetworkRequest http(QUrl(api.baseUrl + u"/models/"_s + bare(model) + method.toString()));
    http.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    // In a header, never in the address, where it would be written to logs.
    http.setRawHeader("x-goog-api-key", api.key.toUtf8());
    http.setTransferTimeout(timeout);
    return http;
}

QJsonObject Gemini::answerBody(const AiRequest &request) {
    QJsonArray contents;
    // Gemini calls the assistant's turns the model's.
    for (const AiMessage &message : request.messages)
        contents.append(QJsonObject{{"role"_L1, message.role == "assistant"_L1 ? u"model"_s : u"user"_s},
                                    {"parts"_L1, QJsonArray{textPart(message.text)}}});
    QJsonObject body{{"contents"_L1, contents}};
    if (!request.system.isEmpty())
        body.insert(u"systemInstruction"_s, QJsonObject{{"parts"_L1, QJsonArray{textPart(request.system)}}});
    return body;
}

QJsonObject Gemini::pictureBody(const QString &prompt, const QByteArray &png) {
    return {{"contents"_L1, QJsonArray{userTurn({textPart(prompt), blobPart(u"image/png"_s, png)})}},
            {"generationConfig"_L1, QJsonObject{{"temperature"_L1, 0}}}};
}

QJsonObject Gemini::recordingBody(const QString &model, const QByteArray &audio, const QString &mimeType, const QString &language) {
    // A transcription model is told how in its configuration, and gives each
    // word its moment; any other model that hears is asked in words.
    if (bare(model).contains("transcribe"_L1)) {
        QJsonObject how{{"wordTimestamp"_L1, true}};
        if (!language.isEmpty())
            how.insert(u"languageCodes"_s, QJsonArray{language});
        return {{"contents"_L1, QJsonArray{userTurn({blobPart(mimeType, audio)})}},
                {"generationConfig"_L1, QJsonObject{{"audioTranscriptionConfig"_L1, how}}}};
    }
    QString prompt = u"Generate a transcript of the speech. Answer with the words said alone."_s;
    if (!language.isEmpty())
        prompt += u" It is most likely in the language whose code is \"%1\"."_s.arg(language);
    return {{"contents"_L1, QJsonArray{userTurn({textPart(prompt), blobPart(mimeType, audio)})}}};
}

QJsonObject Gemini::speechBody(const QString &sentence, const QString &voice) {
    const QString named = voices().contains(voice) ? voice : QString::fromLatin1(defaultVoice);
    const QJsonObject speech{{"voiceConfig"_L1, QJsonObject{{"prebuiltVoiceConfig"_L1, QJsonObject{{"voiceName"_L1, named}}}}}};
    return {{"contents"_L1, QJsonArray{userTurn({textPart(sentence)})}},
            {"generationConfig"_L1, QJsonObject{{"responseModalities"_L1, QJsonArray{u"AUDIO"_s}}, {"speechConfig"_L1, speech}}}};
}

QJsonObject Gemini::embedBody(const QString &model, const QStringList &texts, Use use) {
    QJsonObject config{{"outputDimensionality"_L1, 768}};
    // gemini-embedding-2 takes no task type; the others find more with one.
    if (!bare(model).startsWith("gemini-embedding-2"_L1))
        config.insert(u"taskType"_s, use == Use::Question ? u"RETRIEVAL_QUERY"_s : u"RETRIEVAL_DOCUMENT"_s);
    QJsonArray requests;
    for (const QString &text : texts)
        requests.append(QJsonObject{{"model"_L1, u"models/"_s + bare(model)},
                                    {"content"_L1, QJsonObject{{"parts"_L1, QJsonArray{textPart(text)}}}},
                                    {"embedContentConfig"_L1, config}});
    return {{"requests"_L1, requests}};
}

QString Gemini::textOf(const QJsonObject &response) {
    QString text;
    for (const auto &value : partsOf(response)) {
        const QJsonObject part = value.toObject();
        // A thinking model's working is never the answer.
        if (!part.value("thought"_L1).toBool(false))
            text += part.value("text"_L1).toString();
    }
    return text;
}

bool Gemini::declined(const QJsonObject &response) {
    if (!response.value("promptFeedback"_L1).toObject().value("blockReason"_L1).toString().isEmpty())
        return true;
    static const QStringList refusals{u"SAFETY"_s, u"PROHIBITED_CONTENT"_s, u"BLOCKLIST"_s, u"SPII"_s, u"RECITATION"_s};
    return refusals.contains(response.value("candidates"_L1).toArray().at(0).toObject().value("finishReason"_L1).toString());
}

Recognition::Result Gemini::transcriptOf(const QJsonObject &response) {
    Recognition::Result result{.kind = Recognition::Kind::Audio,
                               .text = {},
                               .segments = {},
                               .language = {},
                               .model = response.value("modelVersion"_L1).toString(),
                               .server = {},
                               .at = QDateTime::currentMSecsSinceEpoch(),
                               .kept = false,
                               .timed = false};
    QStringList said;
    // A transcription model answers a part per stretch, each with its words;
    // another model only the text.
    for (const auto &value : partsOf(response)) {
        const QJsonObject part = value.toObject();
        const QJsonObject heard = part.value("audioTranscription"_L1).toObject();
        Recognition::Segment segment{
            .text = heard.value("text"_L1).toString(part.value("text"_L1).toString()).trimmed(), .start = 0, .end = 0, .words = {}};
        QStringList words;
        for (const auto &word : heard.value("words"_L1).toArray()) {
            const QJsonObject one = word.toObject();
            segment.words.append({.text = one.value("word"_L1).toString().trimmed(),
                                  .start = msOf(one.value("startOffset"_L1)),
                                  .end = msOf(one.value("endOffset"_L1))});
            words << segment.words.last().text;
        }
        if (segment.text.isEmpty())
            segment.text = words.join(u' ');
        if (!segment.words.isEmpty()) {
            segment.start = segment.words.first().start;
            segment.end = segment.words.last().end;
            result.timed = true;
        }
        if (segment.text.isEmpty() || part.value("thought"_L1).toBool(false))
            continue;
        said << segment.text;
        result.segments.append(segment);
    }
    result.text = said.join(u' ');
    return result;
}

QByteArray Gemini::soundOf(const QJsonObject &response) {
    for (const auto &value : partsOf(response)) {
        const QJsonObject blob = value.toObject().value("inlineData"_L1).toObject();
        QByteArray sound = QByteArray::fromBase64(blob.value("data"_L1).toString().toLatin1());
        if (sound.isEmpty())
            continue;
        if (sound.startsWith("RIFF"))
            return sound;
        // Raw PCM, its rate in its type ("audio/L16;codec=pcm;rate=24000").
        static const QRegularExpression rate(u"rate=(\\d+)"_s);
        const auto found = rate.match(blob.value("mimeType"_L1).toString());
        return wav(sound, found.hasMatch() ? found.captured(1).toUInt() : 24000U);
    }
    return {};
}

std::vector<std::vector<float>> Gemini::vectorsOf(const QJsonObject &response) {
    std::vector<std::vector<float>> vectors;
    for (const auto &value : response.value("embeddings"_L1).toArray()) {
        const QJsonArray numbers = value.toObject().value("values"_L1).toArray();
        std::vector<float> vector;
        vector.reserve(static_cast<size_t>(numbers.size()));
        for (const auto &number : numbers)
            vector.push_back(static_cast<float>(number.toDouble()));
        vectors.push_back(std::move(vector));
    }
    return vectors;
}

bool Gemini::keyRefused(int status, const QJsonObject &body) {
    if (status == 401 || status == 403)
        return true;
    const QJsonArray details = body.value("error"_L1).toObject().value("details"_L1).toArray();
    return status == 400 && std::ranges::any_of(details, [](const QJsonValue &detail) {
               return detail.toObject().value("reason"_L1).toString() == "API_KEY_INVALID"_L1;
           });
}

Failure Gemini::failureOf(const Api &api, int status, const QNetworkReply &reply, const QJsonObject &body, const QString &model) {
    if (keyRefused(status, body))
        return Failure(Failure::Code::GeminiRefusedKey);
    return AiFailures::ofServer({.where = api.host(),
                                 .status = status,
                                 .error = reply.error(),
                                 .errorString = reply.errorString(),
                                 .said = AiFailures::serverSaid(body),
                                 .model = bare(model),
                                 .keySet = true});
}

std::expected<std::vector<std::vector<float>>, Failure> Gemini::embed(const Api &api, const QString &model, const QStringList &texts,
                                                                      Use use, std::chrono::milliseconds timeout) {
    // A batch takes at most a hundred texts.
    constexpr qsizetype most = 100;
    std::vector<std::vector<float>> all;
    QNetworkAccessManager network;
    for (qsizetype from = 0; from < texts.size(); from += most) {
        const QStringList some = texts.mid(from, most);
        QNetworkReply *reply = network.post(request(api, model, u":batchEmbedContents", timeout),
                                            QJsonDocument(embedBody(model, some, use)).toJson(QJsonDocument::Compact));
        // On a thread of its own, or the command line's, which has nothing else to do meanwhile.
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (status != 200)
            return std::unexpected(failureOf(api, status, *reply, body, model));
        auto vectors = vectorsOf(body);
        if (std::cmp_not_equal(vectors.size(), some.size()))
            return std::unexpected(Failure(Failure::Code::MeaningNoVectors, {u"Gemini"_s, bare(model)}));
        std::ranges::move(vectors, std::back_inserter(all));
    }
    return all;
}
