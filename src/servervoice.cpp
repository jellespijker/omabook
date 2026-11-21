#include "servervoice.h"

#include <QFile>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QUrl>
#include <algorithm>
#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

ServerVoice::ServerVoice(AiServer server, QString model, QObject *parent)
    : Voice(parent), m_server(std::move(server)), m_model(model.isEmpty() ? QString::fromLatin1(defaultModel) : std::move(model)) {
    connect(&m_sound, &SoundOut::ended, this, &Voice::spoken);
    connect(&m_sound, &SoundOut::failed, this, [this](const QString &why) { emit failed(Failure(Failure::Code::VoiceUnplayed, {why})); });
}

ServerVoice::~ServerVoice() { stop(); }

QStringList ServerVoice::voices() { return {u"alloy"_s, u"echo"_s, u"fable"_s, u"nova"_s, u"onyx"_s, u"shimmer"_s}; }

QJsonObject ServerVoice::request(const QString &model, const QString &sentence, const Speech &how) {
    return {{"model"_L1, model},
            {"input"_L1, sentence},
            {"voice"_L1, how.voice.isEmpty() ? voices().first() : how.voice},
            {"speed"_L1, std::clamp(how.rate, 0.5, 2.0)},
            {"response_format"_L1, u"wav"_s}};
}

void ServerVoice::say(const QString &sentence, const Speech &how) {
    stop();
    QNetworkRequest http(QUrl(m_server.baseUrl + u"/audio/speech"_s));
    if (const QString key = m_server.currentKey(); !key.isEmpty())
        http.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    http.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    http.setTransferTimeout(60s);
    m_reply = m_network.post(http, QJsonDocument(request(m_model, sentence, how)).toJson(QJsonDocument::Compact));
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { heard(reply); });
}

void ServerVoice::heard(QNetworkReply *reply) {
    reply->deleteLater();
    if (reply != m_reply)
        return;
    m_reply = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray sound = reply->readAll();
    if (reply->error() != QNetworkReply::NoError || status != 200 || sound.isEmpty()) {
        const QString why = status > 0 ? QString::number(status) : reply->errorString();
        emit failed(Failure(Failure::Code::VoiceServerFailed, {name(), why}));
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

void ServerVoice::stop() {
    m_sound.stop();
    if (QNetworkReply *reply = m_reply) {
        m_reply = nullptr;
        reply->abort();
    }
}
