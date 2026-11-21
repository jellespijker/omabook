#include "geminivoice.h"

#include <QFile>
#include <QJsonDocument>
#include <chrono>
#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

GeminiVoice::GeminiVoice(Gemini::Api api, QString model, QObject *parent)
    : Voice(parent), m_api(std::move(api)), m_model(model.isEmpty() ? QString::fromLatin1(Gemini::voiceModel) : std::move(model)) {
    connect(&m_sound, &SoundOut::ended, this, &Voice::spoken);
    connect(&m_sound, &SoundOut::failed, this, [this](const QString &why) { emit failed(Failure(Failure::Code::VoiceUnplayed, {why})); });
}

GeminiVoice::~GeminiVoice() { stop(); }

QString GeminiVoice::name() const { return u"Gemini"_s; }

void GeminiVoice::say(const QString &sentence, const Speech &how) {
    stop();
    m_reply = m_network.post(Gemini::request(m_api, m_model, u":generateContent", 60s),
                             QJsonDocument(Gemini::speechBody(sentence, how.voice)).toJson(QJsonDocument::Compact));
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { heard(reply); });
}

void GeminiVoice::heard(QNetworkReply *reply) {
    reply->deleteLater();
    if (reply != m_reply)
        return;
    m_reply = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    if (reply->error() != QNetworkReply::NoError || status != 200) {
        if (Gemini::keyRefused(status, body))
            emit failed(Failure(Failure::Code::GeminiRefusedKey));
        else
            emit failed(Failure(Failure::Code::VoiceServerFailed, {name(), status > 0 ? QString::number(status) : reply->errorString()}));
        return;
    }
    const QByteArray sound = Gemini::soundOf(body);
    if (sound.isEmpty()) {
        emit failed(Failure(Failure::Code::GeminiNoSound));
        return;
    }
    QFile file(m_folder.filePath(u"%1.wav"_s.arg(++m_files % 2)));
    if (!file.open(QIODevice::WriteOnly) || file.write(sound) != sound.size()) {
        emit failed(Failure(Failure::Code::VoiceUnplayed, {file.errorString()}));
        return;
    }
    file.close();
    m_sound.play(file.fileName());
}

void GeminiVoice::stop() {
    m_sound.stop();
    if (QNetworkReply *reply = std::exchange(m_reply, nullptr))
        reply->abort();
}
