#include "servervoice.h"

#include <QFile>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QUrl>
#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

ServerVoice::ServerVoice(AiServer server, QString model, QString fallback, QObject *parent)
    : Voice(parent), m_server(std::move(server)), m_model(model.isEmpty() ? QString::fromLatin1(defaultModel) : std::move(model)),
      m_fallback(std::move(fallback)) {
    connect(&m_sound, &SoundOut::ended, this, &Voice::spoken);
    connect(&m_sound, &SoundOut::failed, this, [this](const QString &why) { emit failed(Failure(Failure::Code::VoiceUnplayed, {why})); });
}

ServerVoice::~ServerVoice() { stop(); }

QStringList ServerVoice::voices() { return {u"alloy"_s, u"echo"_s, u"fable"_s, u"nova"_s, u"onyx"_s, u"shimmer"_s}; }

QStringList ServerVoice::openAiVoices() {
    return {u"alloy"_s, u"ash"_s,  u"ballad"_s,  u"coral"_s, u"echo"_s,  u"fable"_s, u"nova"_s,
            u"onyx"_s,  u"sage"_s, u"shimmer"_s, u"verse"_s, u"marin"_s, u"cedar"_s};
}

QJsonObject ServerVoice::request(const QString &model, const QString &sentence, const Speech &how) {
    return {{"model"_L1, model},
            {"input"_L1, sentence},
            {"voice"_L1, how.voice.isEmpty() ? voices().first() : how.voice},
            {"speed"_L1, std::clamp(how.rate, 0.5, 2.0)},
            {"response_format"_L1, u"wav"_s}};
}

bool ServerVoice::modelGone(int status, const QByteArray &answer) {
    if (status != 400 && status != 404 && status != 410)
        return false;
    return modelGone(QJsonDocument::fromJson(answer).object().value("error"_L1).toObject());
}

bool ServerVoice::modelGone(const QJsonObject &error) {
    const QString code = error.value("code"_L1).toString();
    if (code == "model_not_found"_L1 || code == "model_retired"_L1 || code == "model_deprecated"_L1)
        return true;
    const QString words = error.value("message"_L1).toString().toLower();
    return words.contains("model"_L1)
           && std::ranges::any_of(std::array{"retired"_L1, "deprecated"_L1, "shut down"_L1, "does not exist"_L1, "not found"_L1},
                                  [&words](QLatin1StringView word) { return words.contains(word); });
}

void ServerVoice::say(const QString &sentence, const Speech &how) {
    stop();
    m_sentence = sentence;
    m_how = how;
    ask();
}

void ServerVoice::ask() {
    QNetworkRequest http(QUrl(m_server.baseUrl + u"/audio/speech"_s));
    if (const QString key = m_server.currentKey(); !key.isEmpty())
        http.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    http.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    http.setTransferTimeout(60s);
    m_reply = m_network.post(http, QJsonDocument(request(m_model, m_sentence, m_how)).toJson(QJsonDocument::Compact));
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
    if (modelGone(status, sound)) {
        if (m_fallback.isEmpty() || m_model == m_fallback) {
            emit failed(Failure(Failure::Code::VoiceModelRetired, {name(), m_model}));
            return;
        }
        const QString gone = std::exchange(m_model, m_fallback);
        emit modelRetired(gone);
        ask();
        return;
    }
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
