// KOReader's progress sync (#71): a book known by the hash KOReader names its
// file by, its place said in KOReader's terms and back, Omabook serving the
// sync to a stand-in KOReader, and Omabook syncing as a client with a
// stand-in sync server (its own, served from another library). And the
// server listening only where it is allowed to.

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <array>

#include "deviceoptions.h"
#include "deviceserver.h"
#include "fakedevice.h"
#include "fixtures.h"
#include "koreaderbooks.h"
#include "koreaderdocument.h"
#include "koreadersync.h"
#include "kosyncclient.h"
#include "library.h"

using Fixtures::whyNot;

namespace {

const QByteArray key = KosyncClient::keyOf("secret").toUtf8();

// A book of three chapters, the last twice as long as the others.
[[nodiscard]] qint64 threeChapters(Library &library, const QString &folder) {
    const QString words(400, 'w');
    const QString path = folder + "/three.epub";
    if (!Fixtures::chapteredEpub(
            path, "Three Chapters",
            {{"One", "<p>" + words + "</p>"}, {"Two", "<p>" + words + "</p>"}, {"Three", "<p>" + words + words + "</p>"}}))
        return 0;
    return library.add(path).id;
}

[[nodiscard]] DeviceServer::Setup serving(bool koreader, bool opds = false) {
    DeviceOptions options;
    options.reach = DeviceOptions::Reach::ThisComputer;
    options.port = 0;
    options.opds = opds;
    options.koreaderServe = koreader;
    options.koreaderServeUser = "reader";
    return {.options = options, .opdsToken = "token", .koreaderKey = QString::fromUtf8(key)};
}

[[nodiscard]] QByteArray body(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

} // namespace

class DevicesTest : public QObject {
    Q_OBJECT

private slots:
    void aFileIsKnownByKoreadersPartialMd5() {
        QTemporaryDir dir;
        QByteArray pattern(300000, Qt::Uninitialized);
        for (qsizetype i = 0; i < pattern.size(); ++i)
            pattern[i] = static_cast<char>(i % 251);
        QVERIFY(Fixtures::write(dir.filePath("long"), pattern));
        QVERIFY(Fixtures::write(dir.filePath("short"), "abc"));
        QVERIFY(Fixtures::write(dir.filePath("empty"), {}));
        // 1 KiB at 0, 1, 4, 16, 64 and 256 KiB of the long one; the short one whole.
        QCOMPARE(KoreaderDocument::hash(dir.filePath("long")), QString("c43e7af7c64be64ff8765e78ee771294"));
        QCOMPARE(KoreaderDocument::hash(dir.filePath("short")), QString("900150983cd24fb0d6963f7d28e17f72"));
        QCOMPARE(KoreaderDocument::hash(dir.filePath("empty")), QString("d41d8cd98f00b204e9800998ecf8427e"));
        QVERIFY(KoreaderDocument::hash(dir.filePath("missing")).isEmpty());
    }

    void aPlaceIsSaidInKoreadersTermsAndBack() {
        constexpr std::array<qsizetype, 3> sizes{100, 100, 200};
        // An EPUB's DocFragment is its spine item; how far into it, the percentage weighed by the sizes.
        BookPlace place = KoreaderDocument::placeOf(u"/body/DocFragment[3]/body/p[2]/text().0", 0.75, "epub", sizes);
        QCOMPARE(place.section, 2);
        QCOMPARE(place.fraction, 0.5);
        // One past the last is held to it.
        QCOMPARE(KoreaderDocument::placeOf(u"/body/DocFragment[9]/body", 1, "epub", sizes).section, 2);
        QCOMPARE(KoreaderDocument::placeOf(u"/FictionBook/body/section[2]/p[4]", 0.25, "fb2", sizes).section, 1);
        // A page number, from 1, for a paged book.
        place = KoreaderDocument::placeOf(u"12", 0.5, "pdf", sizes);
        QCOMPARE(place.section, 11);
        QCOMPARE(place.fraction, 0.0);
        // Text that says no section is placed by the percentage alone.
        place = KoreaderDocument::placeOf(u"/body/p[40]", 0.375, "txt", sizes);
        QCOMPARE(place.section, 1);
        QCOMPARE(place.fraction, 0.5);

        QCOMPARE(KoreaderDocument::progressOf({2, 0.5}, "epub"), std::optional<QString>("/body/DocFragment[3]/body"));
        QCOMPARE(KoreaderDocument::progressOf({1, 0}, "fb2"), std::optional<QString>("/FictionBook/body/section[2]"));
        QCOMPARE(KoreaderDocument::progressOf({4, 0}, "pdf"), std::optional<QString>("5"));
        QVERIFY(!KoreaderDocument::progressOf({0, 0.5}, "txt"));
    }

    void koreaderLogsInToOmabookAndSyncsItsPlace() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = threeChapters(library, dir.path());
        QVERIFY(id > 0);
        const QString document = KoreaderDocument::hash(library.formatPath(id, "epub"));
        DeviceServer server(library.root());
        QSignalSpy readTo(&server, &DeviceServer::readTo);
        const auto address = server.start(serving(true));
        QVERIFY2(address && address->startsWith("http://127.0.0.1:"), qPrintable(whyNot(address)));
        const QString at = *address;

        QCOMPARE(FakeDevice::ask("GET", at + "/healthcheck").json().value("state").toString(), QString("OK"));
        // Only the one person, by their name and key.
        FakeDevice::Answer answer = FakeDevice::ask("GET", at + "/users/auth", FakeDevice::koreader("reader", "wrong"));
        QCOMPARE(answer.status, 401);
        QCOMPARE(answer.json().value("code").toInt(), 2001);
        QCOMPARE(FakeDevice::ask("GET", at + "/users/auth", FakeDevice::koreader("reader", key)).json().value("authorized").toString(),
                 QString("OK"));
        QCOMPARE(
            FakeDevice::ask("POST", at + "/users/create", FakeDevice::koreader("", ""), body({{"username", "someone"}, {"password", "x"}}))
                .status,
            402);
        QCOMPARE(FakeDevice::ask("POST", at + "/users/create", FakeDevice::koreader("", ""),
                                 body({{"username", "reader"}, {"password", QString::fromUtf8(key)}}))
                     .status,
                 201);
        const QJsonObject progress{{"document", document},
                                   {"progress", "/body/DocFragment[3]/body/p[1]/text().0"},
                                   {"percentage", 0.75},
                                   {"device", "Kobo"},
                                   {"device_id", "KOBO1"}};
        QCOMPARE(FakeDevice::ask("PUT", at + "/syncs/progress", FakeDevice::koreader("reader", "wrong"), body(progress)).status, 401);

        // The place KOReader sends is the book's place here.
        answer = FakeDevice::ask("PUT", at + "/syncs/progress", FakeDevice::koreader("reader", key), body(progress));
        QCOMPARE(answer.status, 200);
        QCOMPARE(answer.json().value("document").toString(), document);
        QCOMPARE(readTo.size(), 1);
        QCOMPARE(readTo.first().first().toLongLong(), id);
        const Progress there = library.progress(id);
        QCOMPARE(there.place.section, 2);
        QVERIFY2(there.place.fraction > 0 && there.place.fraction < 1, qPrintable(QString::number(there.place.fraction)));
        QCOMPARE(qRound(there.percent), 75);
        // Asked again, it is the device's own record, so KOReader knows it as its own.
        QJsonObject got = FakeDevice::ask("GET", at + "/syncs/progress/" + document, FakeDevice::koreader("reader", key)).json();
        QCOMPARE(got.value("device_id").toString(), QString("KOBO1"));
        QCOMPARE(got.value("progress").toString(), progress.value("progress").toString());

        // Read further here, the place here is the newer, in KOReader's terms.
        library.setProgress(id, {.place = {1, 0.5}, .percent = 40, .updated = QDateTime::currentSecsSinceEpoch() + 60});
        got = FakeDevice::ask("GET", at + "/syncs/progress/" + document, FakeDevice::koreader("reader", key)).json();
        QCOMPARE(got.value("device").toString(), QString("Omabook"));
        QCOMPARE(got.value("progress").toString(), QString("/body/DocFragment[2]/body"));
        QCOMPARE(got.value("percentage").toDouble(), 0.4);
        // A document the library doesn't have is one nothing is known of.
        QVERIFY(FakeDevice::ask("GET", at + "/syncs/progress/0123456789abcdef", FakeDevice::koreader("reader", key)).json().isEmpty());
        // And the OPDS catalog isn't served when it is off.
        QCOMPARE(FakeDevice::ask("GET", at + "/opds", {{"Authorization", "Bearer token"}}).status, 404);
    }

    void omabookSyncsWithAKoreaderSyncServer() {
        // The stand-in sync server: Omabook's own, serving an empty library.
        QTemporaryDir dir;
        Library served(dir.filePath("Served"));
        QVERIFY(served.open());
        DeviceServer server(served.root());
        const auto address = server.start(serving(true));
        QVERIFY2(address, qPrintable(whyNot(address)));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = threeChapters(library, dir.path());
        const QString document = KoreaderDocument::hash(library.formatPath(id, "epub"));
        library.setProgress(id, {.place = {1, 0.5}, .percent = 40, .updated = QDateTime::currentSecsSinceEpoch() - 100});
        QNetworkAccessManager network;

        KosyncClient wrong(network, QUrl(*address), "reader", "nope");
        const auto refused = KoreaderSync::run(library, wrong);
        QVERIFY(!refused);
        QCOMPARE(refused.error().code(), Failure::Code::SyncServerRefused);

        KosyncClient client(network, QUrl(*address), "reader", QString::fromUtf8(key));
        auto summary = KoreaderSync::run(library, client);
        QVERIFY2(summary, qPrintable(whyNot(summary)));
        QCOMPARE(summary->sent, 1);
        QCOMPARE(summary->taken, 0);
        // The server has the place, said as KOReader says it.
        QJsonObject got = FakeDevice::ask("GET", *address + "/syncs/progress/" + document, FakeDevice::koreader("reader", key)).json();
        QCOMPARE(got.value("progress").toString(), QString("/body/DocFragment[2]/body"));
        QCOMPARE(got.value("device_id").toString(), KoreaderBooks::deviceId(library));

        // What Omabook sent isn't taken back as a device's.
        summary = KoreaderSync::run(library, client);
        QVERIFY2(summary, qPrintable(whyNot(summary)));
        QCOMPARE(summary->taken + summary->sent, 0);

        // A device reads further; the next sync takes its place.
        const QJsonObject progress{{"document", document},
                                   {"progress", "/body/DocFragment[3]/body/p[1]/text().0"},
                                   {"percentage", 0.9},
                                   {"device", "Kobo"},
                                   {"device_id", "KOBO1"}};
        QCOMPARE(FakeDevice::ask("PUT", *address + "/syncs/progress", FakeDevice::koreader("reader", key), body(progress)).status, 200);
        summary = KoreaderSync::run(library, client, {id});
        QVERIFY2(summary, qPrintable(whyNot(summary)));
        QCOMPARE(summary->taken, 1);
        QCOMPARE(library.progress(id).place.section, 2);
        QCOMPARE(qRound(library.progress(id).percent), 90);
    }

    void theServerListensOnlyWhereItIsAllowed() {
        QVERIFY(DeviceReach::onTailnet(QHostAddress("100.101.102.103")));
        QVERIFY(!DeviceReach::onTailnet(QHostAddress("192.168.1.20")));
        QVERIFY(!DeviceReach::address(DeviceOptions::Reach::Off));
        QCOMPARE(*DeviceReach::address(DeviceOptions::Reach::ThisComputer), QHostAddress(QHostAddress::LocalHost));
        // Not on a tailnet, there is nowhere there to listen.
        QVERIFY(!DeviceReach::address(DeviceOptions::Reach::Tailnet, {}));
        QTemporaryDir dir;
        DeviceServer server(dir.filePath("Library"));
        DeviceServer::Setup setup = serving(true);
        setup.options.reach = DeviceOptions::Reach::Tailnet;
        const auto none = server.start(setup, {});
        QVERIFY(!none);
        QCOMPARE(none.error().code(), Failure::Code::NoTailnet);
        QVERIFY(!server.running());
        // Off, nothing listens.
        setup.options.reach = DeviceOptions::Reach::Off;
        const auto off = server.start(setup);
        QVERIFY(off && off->isEmpty());
        QVERIFY(!server.running());
    }

    void theOptionsAreOffUntilChosen() {
        const DeviceOptions none = DeviceOptions::fromJson({});
        QCOMPARE(none.reach, DeviceOptions::Reach::Off);
        QVERIFY(!none.koreaderServe);
        DeviceOptions options;
        options.reach = DeviceOptions::Reach::Tailnet;
        options.collection = "Study";
        options.mailStartTls = false;
        const DeviceOptions again = DeviceOptions::fromJson(options.toJson());
        QCOMPARE(again.reach, DeviceOptions::Reach::Tailnet);
        QCOMPARE(again.collection, QString("Study"));
        QVERIFY(!again.mailStartTls);
        // A port typed into the window comes as text.
        QCOMPARE(DeviceOptions::fromJson({{"port", "7000"}}).port, quint16{7000});
        QCOMPARE(DeviceOptions::fromJson({{"port", "none"}}).port, quint16{6622});
    }
};

QTEST_GUILESS_MAIN(DevicesTest)
#include "devices_tests.moc"
