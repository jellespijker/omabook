#include "realtimevoice.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QtEndian>
#include <algorithm>
#include <utility>
#include <vector>

#include "dictationclip.h"
#include "servervoice.h"

using namespace Qt::StringLiterals;

namespace {

// The voice it is told to read with: one of its own, else its default.
[[nodiscard]] QString voiceOf(const Speech &how) {
    return RealtimeVoice::voices().contains(how.voice) ? how.voice : QString::fromLatin1(RealtimeVoice::defaultVoice);
}

// Omabook's pace is half to twice the voice's own; the Realtime API's is 0.25 to 1.5.
[[nodiscard]] double speedOf(const Speech &how) { return std::clamp(how.rate, 0.5, 1.5); }

[[nodiscard]] bool sameSession(const Speech &one, const Speech &other) {
    return voiceOf(one) == voiceOf(other) && speedOf(one) == speedOf(other);
}

[[nodiscard]] QString compact(const QJsonObject &event) { return QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)); }

} // namespace

RealtimeVoice::RealtimeVoice(AiServer server, QUrl address, QString model, std::unique_ptr<SoundOut> sound, QObject *parent)
    : Voice(parent), m_server(std::move(server)), m_address(std::move(address)),
      m_model(model.isEmpty() ? QString::fromLatin1(openAiModel) : std::move(model)),
      m_sound(sound ? std::move(sound) : std::make_unique<SoundOut>()) {
    connect(m_sound.get(), &SoundOut::ended, this, &Voice::spoken);
    connect(m_sound.get(), &SoundOut::failed, this,
            [this](const QString &why) { emit failed(Failure(Failure::Code::VoiceUnplayed, {why})); });
}

RealtimeVoice::~RealtimeVoice() {
    stop();
    m_socket.disconnect(this);
    m_socket.abort();
}

QStringList RealtimeVoice::voices() {
    return {u"alloy"_s, u"ash"_s, u"ballad"_s, u"coral"_s, u"echo"_s, u"sage"_s, u"shimmer"_s, u"verse"_s, u"marin"_s, u"cedar"_s};
}

bool RealtimeVoice::realtimeOnly(const QString &model) { return model.trimmed().startsWith("gpt-realtime"_L1); }

QUrl RealtimeVoice::addressOf(const QString &base) {
    QUrl address(base + u"/realtime"_s);
    address.setScheme(address.scheme() == "http"_L1 ? u"ws"_s : u"wss"_s);
    return address;
}

QJsonObject RealtimeVoice::session(const Speech &how) {
    const QJsonObject output{
        {"format"_L1, QJsonObject{{"type"_L1, u"audio/pcm"_s}, {"rate"_L1, rate}}}, {"voice"_L1, voiceOf(how)}, {"speed"_L1, speedOf(how)}};
    return {{"type"_L1, u"session.update"_s},
            {"session"_L1, QJsonObject{{"type"_L1, u"realtime"_s},
                                       {"instructions"_L1, u"You read text aloud. Read the text you are given exactly as it is "
                                                           u"written, word for word, in its own language, and nothing else: "
                                                           u"don't answer it, explain it or add to it."_s},
                                       {"output_modalities"_L1, QJsonArray{u"audio"_s}},
                                       {"audio"_L1, QJsonObject{{"output"_L1, output}}}}}};
}

QJsonObject RealtimeVoice::response(const QString &sentence) {
    const QJsonObject text{{"type"_L1, u"input_text"_s}, {"text"_L1, sentence}};
    const QJsonObject message{{"type"_L1, u"message"_s}, {"role"_L1, u"user"_s}, {"content"_L1, QJsonArray{text}}};
    return {
        {"type"_L1, u"response.create"_s},
        {"response"_L1,
         QJsonObject{{"conversation"_L1, u"none"_s}, {"output_modalities"_L1, QJsonArray{u"audio"_s}}, {"input"_L1, QJsonArray{message}}}}};
}

void RealtimeVoice::say(const QString &sentence, const Speech &how) {
    stop();
    m_sentence = sentence;
    m_asking = true;
    m_pcm.clear();
    if (m_socket.state() != QAbstractSocket::UnconnectedState && sameSession(m_session, how)) {
        if (m_ready)
            ask();
        return; // asked once the session is there
    }
    m_session = how;
    open();
}

// A session of its own: the one before is let go first, unheard.
void RealtimeVoice::open() {
    m_ready = false;
    m_socket.disconnect(this);
    m_socket.abort();
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &RealtimeVoice::received);
    connect(&m_socket, &QWebSocket::errorOccurred, this,
            [this] { fail(Failure(Failure::Code::VoiceServerFailed, {name(), m_socket.errorString()})); });
    connect(&m_socket, &QWebSocket::disconnected, this, [this] {
        m_ready = false;
        fail(Failure(Failure::Code::VoiceServerFailed,
                     {name(), m_socket.closeReason().isEmpty() ? m_socket.errorString() : m_socket.closeReason()}));
    });
    QUrl address = m_address;
    QUrlQuery query(address);
    query.addQueryItem(u"model"_s, m_model);
    address.setQuery(query);
    QNetworkRequest request(address);
    if (const QString key = m_server.currentKey(); !key.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    m_socket.open(request);
}

void RealtimeVoice::ask() { m_socket.sendTextMessage(compact(response(m_sentence))); }

void RealtimeVoice::received(const QString &message) {
    const QJsonObject event = QJsonDocument::fromJson(message.toUtf8()).object();
    const QString type = event.value("type"_L1).toString();
    if (type == "session.created"_L1) {
        m_ready = true;
        m_socket.sendTextMessage(compact(session(m_session)));
        if (m_asking)
            ask();
    } else if (!m_asking) {
        return; // what is left of a sentence stopped
    } else if (type == "response.output_audio.delta"_L1) {
        m_pcm += QByteArray::fromBase64(event.value("delta"_L1).toString().toLatin1());
    } else if (type == "response.done"_L1) {
        done(event.value("response"_L1).toObject());
    } else if (type == "error"_L1) {
        refused(event.value("error"_L1).toObject());
    }
}

void RealtimeVoice::done(const QJsonObject &response) {
    const QString status = response.value("status"_L1).toString();
    const QJsonObject details = response.value("status_details"_L1).toObject();
    if (status == "failed"_L1) {
        refused(details.value("error"_L1).toObject());
    } else if (status != "completed"_L1) {
        const QString reason = details.value("reason"_L1).toString();
        fail(Failure(Failure::Code::VoiceServerFailed, {name(), reason.isEmpty() ? status : reason}));
    } else if (m_pcm.isEmpty()) {
        fail(Failure(Failure::Code::VoiceNoSound, {name()}));
    } else {
        play();
    }
}

void RealtimeVoice::refused(const QJsonObject &error) {
    const QString code = error.value("code"_L1).toString();
    if (ServerVoice::modelGone(error))
        fail(Failure(Failure::Code::VoiceModelRetired, {name(), m_model}));
    else if (code == "invalid_api_key"_L1 || error.value("type"_L1).toString() == "authentication_error"_L1)
        fail(Failure(Failure::Code::VoiceKeyRefused, {name()}));
    else
        fail(Failure(Failure::Code::VoiceServerFailed, {name(), error.value("message"_L1).toString(code)}));
}

void RealtimeVoice::fail(const Failure &why) {
    if (!std::exchange(m_asking, false))
        return;
    m_pcm.clear();
    emit failed(why);
}

void RealtimeVoice::play() {
    m_asking = false;
    std::vector<qint16> samples(static_cast<std::size_t>(m_pcm.size() / 2));
    qFromLittleEndian<qint16>(m_pcm.constData(), static_cast<qsizetype>(samples.size()), samples.data());
    m_pcm.clear();
    const QByteArray sound = DictationClip::wav(samples, rate);
    QFile file(m_folder.filePath(u"%1.wav"_s.arg(++m_files % 2)));
    if (!file.open(QIODevice::WriteOnly) || file.write(sound) != sound.size()) {
        emit failed(Failure(Failure::Code::VoiceUnplayed, {file.errorString()}));
        return;
    }
    file.close();
    m_sound->play(file.fileName());
}

void RealtimeVoice::stop() {
    m_sound->stop();
    // A sentence still being made: the session is let go, so nothing more of it comes.
    if (std::exchange(m_asking, false)) {
        m_socket.disconnect(this);
        m_socket.abort();
        m_ready = false;
    }
    m_pcm.clear();
}
