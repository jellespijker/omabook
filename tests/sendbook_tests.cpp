// Sending a book to an e-reader (#71): the kind of e-reader known by what it
// keeps in its folder, a book copied onto one mounted there in a format it
// reads, converted first when it has none (with a stand-in for Calibre), and
// a book mailed to a Kindle's address through a stand-in mail server.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtConcurrentRun>
#include <QtTest>
#include <memory>

#include "fixtures.h"
#include "library.h"
#include "sendbook.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

// A mail server that takes everything, or turns the recipient away, and
// keeps what it was told.
class FakeMailServer : public QObject {
public:
    explicit FakeMailServer(bool refuseRecipient = false) : m_refuse(refuseRecipient) {
        if (!m_server.listen(QHostAddress::LocalHost))
            qWarning() << "The stand-in mail server can't listen:" << m_server.errorString();
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->write("220 fake mail\r\n");
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                while (socket->canReadLine())
                    hear(socket, socket->readLine());
            });
        });
    }
    [[nodiscard]] quint16 port() const { return m_server.serverPort(); }
    QByteArray data, login;
    QStringList commands;

private:
    void hear(QTcpSocket *socket, const QByteArray &line) {
        if (m_inData) {
            if (line == ".\r\n") {
                m_inData = false;
                socket->write("250 queued\r\n");
            } else {
                data += line;
            }
            return;
        }
        const QByteArray command = line.trimmed();
        commands << QString::fromUtf8(command.left(4));
        if (command.startsWith("EHLO"))
            socket->write("250-fake mail\r\n250 AUTH PLAIN\r\n");
        else if (command.startsWith("AUTH PLAIN ")) {
            login = QByteArray::fromBase64(command.mid(11));
            socket->write("235 welcome\r\n");
        } else if (command.startsWith("RCPT"))
            socket->write(m_refuse ? "550 no such person\r\n" : "250 ok\r\n");
        else if (command == "DATA") {
            m_inData = true;
            socket->write("354 go on\r\n");
        } else if (command == "QUIT")
            socket->write("221 bye\r\n");
        else
            socket->write("250 ok\r\n");
    }

    QTcpServer m_server;
    bool m_refuse = false;
    bool m_inData = false;
};

// A stand-in for Calibre: the book "converted" to a format, kept with it.
[[nodiscard]] std::expected<QString, Failure> converted(Library &library, qint64 book, const QString &format, const QString &folder) {
    const QString made = folder + "/made." + format;
    if (!Fixtures::write(made, "converted to " + format.toUtf8()))
        return std::unexpected(Failure::said(u"unwritten"_s));
    if (const auto added = library.addFormat(book, made); !added)
        return std::unexpected(added.error());
    return library.formatPath(book, format);
}

} // namespace

class SendBookTest : public QObject {
    Q_OBJECT

private slots:
    void anEreaderIsKnownByWhatItKeeps() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("kobo/.kobo")));
        QVERIFY(QDir().mkpath(dir.filePath("kindle/documents")) && QDir().mkpath(dir.filePath("kindle/system")));
        QVERIFY(QDir().mkpath(dir.filePath("pocketbook/system/config")));
        QVERIFY(QDir().mkpath(dir.filePath("phone")));
        QCOMPARE(SendBook::deviceAt(dir.filePath("kobo")), SendBook::Device::Kobo);
        QCOMPARE(SendBook::deviceAt(dir.filePath("kindle")), SendBook::Device::Kindle);
        QCOMPARE(SendBook::deviceAt(dir.filePath("pocketbook")), SendBook::Device::PocketBook);
        QCOMPARE(SendBook::deviceAt(dir.filePath("phone")), SendBook::Device::Other);
        QCOMPARE(SendBook::booksFolder(dir.filePath("kindle"), SendBook::Device::Kindle), dir.filePath("kindle/documents"));
        // A Kindle never gets an EPUB over USB; the mail takes one.
        QVERIFY(!SendBook::formatsFor(SendBook::Device::Kindle).contains("epub"));
        QCOMPARE(SendBook::choose({"epub", "pdf"}, SendBook::formatsFor(SendBook::Device::Kindle))->format, QString("pdf"));
        QCOMPARE(SendBook::choose({"epub"}, SendBook::formatsFor(SendBook::Device::Kindle))->convert, true);
        QCOMPARE(SendBook::choose({"mobi"}, SendBook::mailFormats())->format, QString("epub"));
        QVERIFY(!SendBook::choose({}, SendBook::mailFormats()));
    }

    void aBookGoesOntoTheMountedEreaderInAFormatItReads() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("alpha.epub"), "Alpha"));
        const qint64 id = library.add(dir.filePath("alpha.epub")).id;
        QVERIFY(QDir().mkpath(dir.filePath("kobo/.kobo")));
        QVERIFY(QDir().mkpath(dir.filePath("kindle/documents")) && QDir().mkpath(dir.filePath("kindle/system")));
        int conversions = 0;
        const SendBook::Convert convert = [&](Library &in, qint64 book, const QString &format) {
            ++conversions;
            return converted(in, book, format, dir.path());
        };

        // A Kobo reads the EPUB as it is.
        const auto onKobo = SendBook::toFolder(library, id, dir.filePath("kobo"), convert);
        QVERIFY2(onKobo, qPrintable(whyNot(onKobo)));
        QCOMPARE(QFileInfo(*onKobo).dir().absolutePath(), QDir(dir.filePath("kobo")).absolutePath());
        QVERIFY2(onKobo->endsWith(".epub") && QFileInfo(*onKobo).fileName().startsWith("Alpha"), qPrintable(*onKobo));
        QFile sent(*onKobo), kept(library.formatPath(id, "epub"));
        QVERIFY(sent.open(QIODevice::ReadOnly) && kept.open(QIODevice::ReadOnly));
        QCOMPARE(sent.readAll(), kept.readAll());
        QCOMPARE(conversions, 0);
        // Sent again, it takes the place of the first copy.
        QVERIFY(SendBook::toFolder(library, id, dir.filePath("kobo"), convert));
        QCOMPARE(QDir(dir.filePath("kobo")).entryList(QDir::Files).size(), 1);

        // A Kindle's is converted first, and the new format stays with the book.
        const auto onKindle = SendBook::toFolder(library, id, dir.filePath("kindle"), convert);
        QVERIFY2(onKindle, qPrintable(whyNot(onKindle)));
        QCOMPARE(conversions, 1);
        QVERIFY2(onKindle->startsWith(dir.filePath("kindle/documents/")) && onKindle->endsWith(".azw3"), qPrintable(*onKindle));
        QVERIFY(library.book(id)->formats.contains("azw3"));

        // Nothing to send, nowhere to send it, or a conversion that fails.
        BookMeta meta;
        meta.title = "On Paper";
        const qint64 paper = library.addEntry(meta, true).id;
        const auto noFile = SendBook::toFolder(library, paper, dir.filePath("kobo"), convert);
        QVERIFY(!noFile);
        QCOMPARE(noFile.error().code(), Failure::Code::NothingToSend);
        const auto unmounted = SendBook::toFolder(library, id, dir.filePath("gone"), convert);
        QVERIFY(!unmounted);
        QCOMPARE(unmounted.error().code(), Failure::Code::NoDeviceFolder);
        QVERIFY(Fixtures::epub(dir.filePath("beta.epub"), "Beta"));
        const qint64 beta = library.add(dir.filePath("beta.epub")).id;
        const auto failing = SendBook::toFolder(library, beta, dir.filePath("kindle"),
                                                [](Library &, qint64, const QString &) -> std::expected<QString, Failure> {
                                                    return std::unexpected(Failure::said(u"ebook-convert stopped"_s));
                                                });
        QVERIFY(!failing);
        QCOMPARE(failing.error().text(), QString("ebook-convert stopped"));
    }

    void aBookIsMailedToAKindle() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("alpha.epub"), "Alpha"));
        const qint64 id = library.add(dir.filePath("alpha.epub")).id;
        const QString root = library.root();
        FakeMailServer mail;
        const SendBook::Post post{.server = {.host = u"127.0.0.1"_s,
                                             .port = mail.port(),
                                             .security = SmtpMail::Server::Security::None,
                                             .user = u"me"_s,
                                             .password = u"pass"_s},
                                  .from = u"me@example.org"_s,
                                  .to = u"me_kindle@kindle.com"_s};
        // The mail is sent on a thread of its own, as the window sends it, while the server answers here.
        const auto send = [root, id](const SendBook::Post &post) {
            Library opened(root);
            if (!opened.open())
                return std::expected<void, Failure>(std::unexpected(Failure::said(u"unopened"_s)));
            return SendBook::byMail(opened, id, post);
        };
        QFuture<std::expected<void, Failure>> sending = QtConcurrent::run(send, post);
        QTRY_VERIFY_WITH_TIMEOUT(sending.isFinished(), 20000);
        QVERIFY2(sending.result(), qPrintable(whyNot(sending.result())));
        QCOMPARE(mail.login, QByteArray("\0me\0pass", 8));
        QVERIFY2(mail.commands.contains("MAIL") && mail.commands.contains("RCPT"), qPrintable(mail.commands.join(' ')));
        QVERIFY(mail.data.contains("To: <me_kindle@kindle.com>"));
        QVERIFY(mail.data.contains("Content-Type: application/epub+zip"));
        QFile kept(library.formatPath(id, "epub"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QVERIFY(mail.data.contains(kept.readAll().toBase64().left(76)));

        // Turned away, it says so; not set up, it says what is missing.
        FakeMailServer refusing(true);
        SendBook::Post refused = post;
        refused.server.port = refusing.port();
        sending = QtConcurrent::run(send, refused);
        QTRY_VERIFY_WITH_TIMEOUT(sending.isFinished(), 20000);
        QVERIFY(!sending.result());
        QCOMPARE(sending.result().error().code(), Failure::Code::MailRefused);
        const auto unset = SendBook::byMail(library, id, {});
        QVERIFY(!unset);
        QCOMPARE(unset.error().code(), Failure::Code::MailNotSetUp);
    }
};

QTEST_GUILESS_MAIN(SendBookTest)
#include "sendbook_tests.moc"
