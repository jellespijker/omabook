#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <expected>

#include "failure.h"

// Sending one mail with a file attached through the person's own mail
// server (#71), as Send-to-Kindle takes a book: SMTP with STARTTLS or TLS
// from the start, and the server's login. It waits for every answer, so it
// is not for the window's thread.
namespace SmtpMail {

struct Server {
    enum class Security : std::uint8_t {
        StartTls,
        Tls,
        None, // only for a server on this computer, as the tests' is
    };
    QString host;
    quint16 port = 587;
    Security security = Security::StartTls;
    QString user;
    QString password;
};

struct Mail {
    QString from;
    QString to;
    QString subject;
    QString text;
    QString attachment; // the file's path
    QString attachmentName;
    QString attachmentType;
};

[[nodiscard]] std::expected<void, Failure> send(const Server &server, const Mail &mail);

// The message as it is sent, headers and all, before the dots of its lines are doubled.
[[nodiscard]] QByteArray message(const Mail &mail, const QByteArray &attached);

} // namespace SmtpMail
