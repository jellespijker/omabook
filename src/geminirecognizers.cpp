#include "geminirecognizers.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkReply>
#include <chrono>
#include <utility>

#include "inkpicture.h"
#include "recognizers.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

using Why = Recognition::Failure::Why;

// Why Gemini didn't answer, as every way of recognising says it: a key it
// refused is said so, the rest as a server's (Recognizers::failure).
[[nodiscard]] Recognition::Failure failureOf(const QNetworkReply &reply, const QByteArray &body, const QString &where, const QString &model,
                                             bool transcribing) {
    const int status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (Gemini::keyRefused(status, QJsonDocument::fromJson(body).object()))
        return {.why = Why::Refused, .message = Failure(Failure::Code::GeminiRefusedKey).text(), .lacking = false};
    return Recognizers::failure(reply, body, where, model, transcribing);
}

[[nodiscard]] bool answered(const QNetworkReply &reply) {
    return reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 && reply.error() == QNetworkReply::NoError;
}

void dropReply(QPointer<QNetworkReply> &held, const QObject *owner) {
    if (QNetworkReply *reply = std::exchange(held, nullptr)) {
        reply->disconnect(owner);
        reply->abort();
        reply->deleteLater();
    }
}

} // namespace

GeminiVision::GeminiVision(Gemini::Api api, QString model, QObject *parent)
    : Recognizer(parent), m_api(std::move(api)), m_model(model.isEmpty() ? QString::fromLatin1(Gemini::visionModel) : std::move(model)) {}

GeminiVision::~GeminiVision() { cancel(); }

QString GeminiVision::where() const { return u"Gemini"_s; }

void GeminiVision::recognize(const Recognition::Input &input) {
    cancel();
    const QByteArray png = Ink::png(input.strokes);
    if (png.isEmpty()) {
        emit failed({.why = Why::Refused, .message = Failure(Failure::Code::NoInk).text(), .lacking = false});
        return;
    }
    m_language = input.language;
    const QJsonObject body = Gemini::pictureBody(Recognizers::inkPrompt(input.language), png);
    m_reply =
        m_network.post(Gemini::request(m_api, m_model, u":generateContent", 5min), QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        if (QNetworkReply *reply = std::exchange(m_reply, nullptr)) {
            reply->deleteLater();
            read(reply);
        }
    });
}

void GeminiVision::read(QNetworkReply *reply) {
    const QByteArray body = reply->readAll();
    if (!answered(*reply)) {
        emit failed(failureOf(*reply, body, where(), m_model, false));
        return;
    }
    const QString text = Gemini::textOf(QJsonDocument::fromJson(body).object()).trimmed();
    emit recognized({.kind = Recognition::Kind::Ink,
                     .text = text,
                     .segments = {{.text = text, .start = 0, .end = 0, .words = {}}},
                     .language = m_language,
                     .model = m_model,
                     .server = where(),
                     .at = QDateTime::currentMSecsSinceEpoch(),
                     .kept = false,
                     .timed = false});
}

void GeminiVision::cancel() { dropReply(m_reply, this); }

GeminiTranscription::GeminiTranscription(Gemini::Api api, QString model, QObject *parent)
    : Recognizer(parent), m_api(std::move(api)),
      m_model(model.isEmpty() ? QString::fromLatin1(Gemini::transcriptionModel) : std::move(model)) {}

GeminiTranscription::~GeminiTranscription() { cancel(); }

QString GeminiTranscription::where() const { return u"Gemini"_s; }

bool GeminiTranscription::timesWords() const { return m_model.contains("transcribe"_L1); }

void GeminiTranscription::recognize(const Recognition::Input &input) {
    cancel();
    QFile file(input.file);
    if (!file.open(QIODevice::ReadOnly)) {
        emit failed({.why = Why::Refused, .message = Failure(Failure::Code::RecordingUnreadable, {input.file}).text(), .lacking = false});
        return;
    }
    // Sent inline, so whole: one too long for a request is said so, never cut.
    if (file.size() > Gemini::largestRecording) {
        emit failed({.why = Why::Refused,
                     .message = Failure(Failure::Code::RecordingTooLarge,
                                        {input.file, QString::number(Gemini::largestRecording / (qsizetype{1024} * 1024))})
                                    .text(),
                     .lacking = false});
        return;
    }
    m_language = input.language;
    const QJsonObject body = Gemini::recordingBody(m_model, file.readAll(), Recognizers::mimeTypeOf(input.file), input.language);
    // An hour of speech takes a while to write out, and the API says nothing meanwhile.
    m_reply =
        m_network.post(Gemini::request(m_api, m_model, u":generateContent", 30min), QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        if (QNetworkReply *reply = std::exchange(m_reply, nullptr)) {
            reply->deleteLater();
            read(reply);
        }
    });
}

void GeminiTranscription::read(QNetworkReply *reply) {
    const QByteArray body = reply->readAll();
    if (!answered(*reply)) {
        emit failed(failureOf(*reply, body, where(), m_model, true));
        return;
    }
    Recognition::Result result = Gemini::transcriptOf(QJsonDocument::fromJson(body).object());
    result.language = m_language;
    result.model = m_model;
    result.server = where();
    emit recognized(result);
}

void GeminiTranscription::cancel() { dropReply(m_reply, this); }
