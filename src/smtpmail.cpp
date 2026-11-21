#include "smtpmail.h"

#include <QDateTime>
#include <QFile>
#include <QSslSocket>
#include <QUrl>
#include <QUuid>
#include <tuple>

using namespace Qt::StringLiterals;

namespace {

constexpr int waitMs = 30'000;

// A conversation with the server, each step an answer it must give.
class Conversation {
public:
    Conversation(QSslSocket &socket, QString where) : m_socket(&socket), m_where(std::move(where)) {}

    // Its reply, all of its lines; none, with the failure kept, when it
    // didn't come, or didn't start with `expected`.
    [[nodiscard]] bool reply(const QByteArray &expected) {
        QByteArray last;
        // A reply of several lines has a '-' after the code on all but its last.
        bool more = true;
        while (more) {
            while (!m_socket->canReadLine()) {
                if (!m_socket->waitForReadyRead(waitMs)) {
                    m_failure = Failure(Failure::Code::MailUnreachable, {m_where, m_socket->errorString()});
                    return false;
                }
            }
            last = m_socket->readLine();
            more = last.size() > 3 && last.at(3) == '-';
        }
        if (last.startsWith(expected))
            return true;
        m_failure = Failure(Failure::Code::MailRefused, {m_where, QString::fromUtf8(last).trimmed()});
        return false;
    }
    [[nodiscard]] bool say(const QByteArray &line, const QByteArray &expected) {
        m_socket->write(line + "\r\n");
        return reply(expected);
    }
    [[nodiscard]] const Failure &failure() const { return m_failure; }

private:
    QSslSocket *m_socket; // never null: what it was made with
    QString m_where;
    Failure m_failure;
};

[[nodiscard]] QByteArray encodedWords(const QString &text) { return "=?UTF-8?B?" + text.toUtf8().toBase64() + "?="; }

[[nodiscard]] QByteArray wrapped(const QByteArray &base64) {
    QByteArray lines;
    for (qsizetype at = 0; at < base64.size(); at += 76)
        lines += base64.mid(at, 76) + "\r\n";
    return lines;
}

// Lines that start with a dot get another, so none ends the message early.
[[nodiscard]] QByteArray dotted(const QByteArray &message) {
    QByteArray out = message;
    out.replace("\r\n.", "\r\n..");
    if (out.startsWith('.'))
        out.prepend('.');
    return out;
}

} // namespace

QByteArray SmtpMail::message(const Mail &mail, const QByteArray &attached) {
    const QByteArray boundary = "omabook-" + QUuid::createUuid().toByteArray(QUuid::Id128);
    QString plainName = mail.attachmentName;
    for (QChar &c : plainName) {
        if (c.unicode() > 127 || c == '"')
            c = '_';
    }
    QByteArray out;
    out += "From: <" + mail.from.toUtf8() + ">\r\n";
    out += "To: <" + mail.to.toUtf8() + ">\r\n";
    out += "Subject: " + encodedWords(mail.subject) + "\r\n";
    out += "Date: " + QDateTime::currentDateTime().toString(Qt::RFC2822Date).toLatin1() + "\r\n";
    out += "MIME-Version: 1.0\r\n";
    out += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n";
    out += "--" + boundary + "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n";
    out += wrapped(mail.text.toUtf8().toBase64());
    out += "--" + boundary + "\r\nContent-Type: " + mail.attachmentType.toLatin1() + "; name=\"" + plainName.toLatin1() + "\"\r\n";
    out += "Content-Disposition: attachment; filename=\"" + plainName.toLatin1() + "\"; filename*=UTF-8''"
           + QUrl::toPercentEncoding(mail.attachmentName) + "\r\n";
    out += "Content-Transfer-Encoding: base64\r\n\r\n";
    out += wrapped(attached.toBase64());
    out += "--" + boundary + "--\r\n";
    return out;
}

namespace {

// Connected, greeted, encrypted and logged in, ready for the mail.
[[nodiscard]] std::expected<void, Failure> openTo(QSslSocket &socket, Conversation &talk, const SmtpMail::Server &server,
                                                  const QString &where) {
    using Security = SmtpMail::Server::Security;
    if (server.security == Security::Tls)
        socket.connectToHostEncrypted(server.host, server.port);
    else
        socket.connectToHost(server.host, server.port);
    const bool connected = server.security == Security::Tls ? socket.waitForEncrypted(waitMs) : socket.waitForConnected(waitMs);
    if (!connected)
        return std::unexpected(Failure(Failure::Code::MailUnreachable, {where, socket.errorString()}));
    if (!talk.reply("220") || !talk.say("EHLO omabook", "250"))
        return std::unexpected(talk.failure());
    if (server.security == Security::StartTls) {
        if (!talk.say("STARTTLS", "220"))
            return std::unexpected(talk.failure());
        socket.startClientEncryption();
        if (!socket.waitForEncrypted(waitMs))
            return std::unexpected(Failure(Failure::Code::MailUnreachable, {where, socket.errorString()}));
        if (!talk.say("EHLO omabook", "250"))
            return std::unexpected(talk.failure());
    }
    if (server.user.isEmpty())
        return {};
    const QByteArray login = '\0' + server.user.toUtf8() + '\0' + server.password.toUtf8();
    if (!talk.say("AUTH PLAIN " + login.toBase64(), "235"))
        return std::unexpected(talk.failure());
    return {};
}

} // namespace

std::expected<void, Failure> SmtpMail::send(const Server &server, const Mail &mail) {
    const QString where = u"%1:%2"_s.arg(server.host).arg(server.port);
    QFile file(mail.attachment);
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(Failure(Failure::Code::FileUnreadable, {file.errorString()}));
    QSslSocket socket;
    Conversation talk(socket, where);
    if (const auto opened = openTo(socket, talk, server, where); !opened)
        return opened;
    if (!talk.say("MAIL FROM:<" + mail.from.toUtf8() + ">", "250") || !talk.say("RCPT TO:<" + mail.to.toUtf8() + ">", "25")
        || !talk.say("DATA", "354"))
        return std::unexpected(talk.failure());
    socket.write(dotted(message(mail, file.readAll())));
    if (!talk.say(".", "250"))
        return std::unexpected(talk.failure());
    std::ignore = talk.say("QUIT", "221"); // The book went; how the server says goodbye doesn't matter.
    return {};
}
