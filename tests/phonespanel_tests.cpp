// The Phones and e-readers panel (#177), from the settings and the library's
// menu: who serves and where, the catalog's address, user name and password
// to copy, and QR codes that a camera reads back to exactly those (read
// here by qrread.h from the panel as drawn); a new password that refuses the
// old one; KOReader's sync server as a code too. Surveyed in German, under
// fingers and right to left.

#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QTcpServer>
#include <QtTest>
#include <utility>

#include "desk.h"
#include "fakedevice.h"
#include "qrread.h"
#include "secretstore.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] quint16 freePort() {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, 0) ? probe.serverPort() : 0;
}

// What a camera reads off a QR code on screen.
[[nodiscard]] std::optional<QByteArray> scanned(const Desk &desk, const QString &name) {
    QQuickItem *item = desk.item(name);
    if (!item || !desk.shows(name))
        return std::nullopt;
    const QSharedPointer<QQuickItemGrabResult> grab = item->grabToImage();
    if (!grab)
        return std::nullopt;
    QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
    if (!ready.wait(3000))
        return std::nullopt;
    // With OMABOOK_SHOTS set to a folder, the code as grabbed, for another reader (zbarimg) to check too.
    if (const QString folder = qEnvironmentVariable("OMABOOK_SHOTS"); !folder.isEmpty())
        grab->image().save(folder + "/" + name + ".png");
    return QrRead::read(grab->image());
}

[[nodiscard]] int statusOf(const QString &url) { return FakeDevice::ask("GET", QUrl(url)).status; }

} // namespace

class PhonesPanelTests : public QObject {
    Q_OBJECT

    static QVariant value(const Desk &desk, const QString &name, const char *property) {
        QObject *found = desk.find(name);
        return found ? found->property(property) : QVariant();
    }
    static void press(const Desk &desk, const QString &name) {
        QObject *button = desk.find(name);
        QVERIFY2(button, qPrintable(name));
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    }
    // Served on this computer, on a port of its own, the catalog and KOReader's sync.
    static quint16 serve(Desk &desk, bool koreader) {
        const quint16 port = freePort();
        desk.devices->set("port", port);
        desk.devices->set("koreader_serve", koreader);
        desk.devices->set("koreader_serve_user", "reader");
        desk.devices->set("serve", "localhost");
        return port;
    }
    static bool open(Desk &desk) {
        desk.call("showPhones");
        return desk.waitForPopup() && value(desk, "phones", "opened").toBool();
    }

    QStringList found;
    void look(Desk &desk, const QString &state) {
        QVERIFY2(desk.pictured(), qPrintable(state));
        Survey survey{desk.window, desk.touch, {}};
        survey.walk();
        const QString where = QString("%1%2 %3x%4 %5")
                                  .arg(QString::fromLatin1(desk.language), desk.touch ? " touch" : "")
                                  .arg(desk.window->width())
                                  .arg(desk.window->height())
                                  .arg(state);
        for (const QString &problem : std::as_const(survey.problems))
            if (!found.contains(where + " — " + problem))
                found << where + " — " + problem;
        desk.shot(QString("phones-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void aPhoneIsConnectedByItsCamera() {
        Desk desk("en", {}, false);
        const quint16 port = serve(desk, true);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QTRY_COMPARE(desk.devices->servedBy(), QString("window"));
        // The settings say whether it is served, and open the panel.
        desk.call("showSettings");
        QVERIFY(desk.waitForPopup());
        QTRY_VERIFY(desk.shows("phonesSummary"));
        QVERIFY2(value(desk, "phonesSummary", "text").toString().startsWith("This window serves the library on this computer only"),
                 qPrintable(value(desk, "phonesSummary", "text").toString()));
        press(desk, "openPhones");
        QTRY_VERIFY(value(desk, "phones", "opened").toBool());
        QTRY_VERIFY(desk.shows("phonesConnect"));

        const QString address = u"http://127.0.0.1:%1"_s.arg(port);
        const QString token = desk.devices->token();
        QVERIFY(!token.isEmpty());
        QCOMPARE(
            value(desk, "phonesStatus", "text").toString(),
            u"This window serves the library on this computer only, on port %1. 0 devices asked it for something in the last day."_s.arg(
                port));
        QCOMPARE(value(desk, "phonesAddress", "value").toString(), address + "/opds");
        QCOMPARE(value(desk, "phonesToken", "value").toString(), token);
        QVERIFY(!value(desk, "phonesUser", "value").toString().isEmpty());
        QVERIFY(!desk.shows("phonesNamedAddress")); // no tailnet name on this computer only
        QTRY_COMPARE(scanned(desk, "phonesCatalogCode"), std::optional<QByteArray>((address + "/opds").toUtf8()));
        QTRY_COMPARE(scanned(desk, "phonesTokenCode"), std::optional<QByteArray>((address + "/t/" + token + "/opds").toUtf8()));
        QTRY_COMPARE(scanned(desk, "phonesSyncCode"), std::optional<QByteArray>(address.toUtf8()));
        QVERIFY(value(desk, "phonesSyncHow", "text").toString().contains(address));
        for (const char *app : {"KOReader", "Librera", "Moon+ Reader"})
            QVERIFY2(value(desk, "phonesApps", "text").toString().contains(app), app);
        // What the code holds is what the catalog answers to.
        QCOMPARE(statusOf(address + "/t/" + token + "/opds"), 200);

        // A new password: the old one is refused, the code holds the new one.
        press(desk, "renewToken");
        QTRY_VERIFY(desk.devices->token() != token && !desk.devices->token().isEmpty());
        const QString renewed = desk.devices->token();
        QTRY_COMPARE(desk.devices->servedBy(), QString("window"));
        QTRY_COMPARE(statusOf(address + "/t/" + renewed + "/opds"), 200);
        QVERIFY(statusOf(address + "/t/" + token + "/opds") >= 400);
        QTRY_COMPARE(scanned(desk, "phonesTokenCode"), std::optional<QByteArray>((address + "/t/" + renewed + "/opds").toUtf8()));
        QCOMPARE(value(desk, "phonesToken", "value").toString(), renewed);

        // A device that fetched is counted.
        desk.devices->refresh();
        QTRY_VERIFY(value(desk, "phonesStatus", "text").toString().contains("1 device asked it"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // The library's menu has it too.
    void theMenuOpensIt() {
        Desk desk("en", {}, false);
        QVERIFY(desk.show());
        desk.menu("library");
        QVERIFY(desk.waitForPopup());
        QObject *menu = desk.find("commandMenu");
        const QVariantList entries = menu->property("entries").toList();
        const auto entry =
            std::ranges::find_if(entries, [](const QVariant &each) { return each.toMap().value("id") == "library.devices"; });
        QVERIFY2(entry != entries.end(), "the library's menu has no Phones and e-readers");
        QVERIFY(QMetaObject::invokeMethod(menu, "take", Q_ARG(QVariant, *entry)));
        QTRY_VERIFY(value(desk, "phones", "opened").toBool());
        // Off until chosen: nothing to connect yet.
        QVERIFY(value(desk, "phonesStatus", "text").toString().startsWith("Not served"));
        QVERIFY(!desk.shows("phonesConnect"));
    }

    // The panel serving, at every size, in a long language, under fingers and right to left.
    void thePanelAtEverySize() {
        for (const auto &[language, touch] : {std::pair{QByteArray("de"), false}, {QByteArray("de"), true}, {QByteArray("ar"), false}}) {
            Desk desk(language, touch ? QByteArray() : lightTheme, touch);
            serve(desk, true);
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            QTRY_COMPARE(desk.devices->servedBy(), QString("window"));
            QVERIFY(open(desk));
            QTRY_VERIFY(desk.shows("phonesConnect"));
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                desk.quicken();
                desk.settle();
                look(desk, "serving");
            }
            QVERIFY(desk.closePopups());
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        const QStringList problems = std::exchange(found, {});
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
    }
};

QTEST_MAIN(PhonesPanelTests)
#include "phonespanel_tests.moc"
