#include "geminiprovider.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <chrono>
#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

GeminiProvider::GeminiProvider(Gemini::Api api, QObject *parent) : AiProvider(parent), m_api(std::move(api)) {}

QString GeminiProvider::name() const { return u"Gemini"_s; }

void GeminiProvider::ask(const AiRequest &request) {
    cancel();
    m_request = request;
    m_answered = false;
    send(modelFor(request));
}

void GeminiProvider::send(const QString &named) {
    // No model named for this kind of work is Gemini's default for answers.
    m_named = named;
    m_sent = named.isEmpty() ? QString::fromLatin1(Gemini::answersModel) : named;
    began(m_request, m_sent);
    m_buffer.clear();
    m_body.clear();
    m_streamError = {};
    m_declined = false;
    // Streamed, so a long answer arrives as it is written and never times out
    // while it is; a quiet stretch of minutes is the network's.
    m_reply = m_network.post(Gemini::request(m_api, m_sent, u":streamGenerateContent?alt=sse", 3min),
                             QJsonDocument(Gemini::answerBody(m_request)).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &GeminiProvider::read);
    connect(m_reply, &QNetworkReply::finished, this, &GeminiProvider::done);
}

void GeminiProvider::cancel() {
    if (!m_reply)
        return;
    QNetworkReply *reply = std::exchange(m_reply, nullptr);
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

// Server-sent events: each `data:` line is one GenerateContentResponse, a
// piece of the answer.
void GeminiProvider::read() {
    const QByteArray chunk = m_reply->readAll();
    if (m_body.size() < 65536)
        m_body += chunk; // an error arrives as a plain JSON body instead
    m_buffer += chunk;
    for (qsizetype end = m_buffer.indexOf('\n'); end >= 0; end = m_buffer.indexOf('\n')) {
        const QByteArray line = m_buffer.left(end).trimmed();
        m_buffer.remove(0, end + 1);
        if (!line.startsWith("data:"))
            continue;
        const QJsonObject event = QJsonDocument::fromJson(line.mid(5).trimmed()).object();
        if (event.contains("error"_L1)) {
            m_streamError = event;
            continue;
        }
        m_declined = m_declined || Gemini::declined(event);
        if (const QString text = Gemini::textOf(event); !text.isEmpty()) {
            m_answered = true;
            emit delta(text);
        }
    }
}

void GeminiProvider::done() {
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    read();
    m_reply = nullptr;
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool failedHere = status != 200 || reply->error() != QNetworkReply::NoError;
    if (!failedHere && m_streamError.isEmpty()) {
        // A refusal is not about which model it was, so not something another try mends.
        if (m_declined && !m_answered)
            emit failed(Failure(Failure::Code::AiDeclined));
        else
            emit finished();
        return;
    }
    // A reply cut off on its way (a timeout, a dropped connection) is the network's doing.
    int said = status;
    if (!failedHere)
        said = 200;
    else if (status == 200)
        said = 0;
    const Failure problem =
        Gemini::failureOf(m_api, said, *reply, failedHere ? QJsonDocument::fromJson(m_body).object() : m_streamError, m_sent);
    // The model asked for, not the one it came to: a default that was
    // refused leaves nothing to try, so this never goes round in circles.
    if (const auto next = !m_answered ? modelToRetry(m_named, problem) : std::nullopt) {
        emit noted(retryNote(m_sent, *next, m_request));
        send(*next);
        return;
    }
    emit failed(problem);
}
