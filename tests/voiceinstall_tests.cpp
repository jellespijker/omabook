// Installing a voice where nothing reads aloud (#156): on Omarchy, R offers
// speech-dispatcher from Arch's packages or Piper from the AUR, with the voice
// of the book's language; the choice runs in Omarchy's floating terminal, and
// once that is closed (this window has the keyboard back) the book is read
// aloud from where it was asked, or it is said that no voice came. Elsewhere,
// what to install is only said, as before. The Aa panel offers the same. And
// voxtype, which hears speech (#146), offered in the settings the same way.
//
// Omarchy's commands are fakes on the path that write down how they were
// called and "install" a fake voice where Voices looks for one; nothing is
// ever installed. OMABOOK_SHOTS=<folder> saves a picture of each, to look at
// by eye.

#include <QQuickStyle>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>

#include "desk.h"
#include "readaloud.h"
#include "speechinstall.h"
#include "voiceinstall.h"

namespace {

// Omarchy's commands for installing, faked: each writes its arguments to
// `log` and puts what its packages would have where it would be. With
// `nothing`, the install installs nothing (it failed, or was given up on).
struct Omarchy {
    QTemporaryDir dir;
    QString bin = dir.filePath("bin");
    QString share = dir.filePath("share");
    QString log = dir.filePath("log");
    QByteArray path = qgetenv("PATH");
    QByteArray dataDirs = qgetenv("XDG_DATA_DIRS");

    explicit Omarchy(bool here = true, bool nothing = false) {
        QDir().mkpath(bin);
        QDir().mkpath(share);
        // Only these on the path: what the machine running the tests has installed is not found.
        qputenv("PATH", bin.toUtf8());
        qputenv("XDG_DATA_DIRS", share.toUtf8());
        if (!here)
            return;
        const QString head = "#!/bin/sh\nPATH=" + bin + ":/usr/bin:/bin\nLOG=" + log + "\n";
        script("omarchy-install-app", head + "printf 'install-app|%s|%s\\n' \"$1\" \"$2\" >> \"$LOG\"\nexec omarchy-pkg-add $2\n");
        script("omarchy-launch-floating-terminal-with-presentation",
               head + "printf 'floating|%s\\n' \"$*\" >> \"$LOG\"\nexec /bin/sh -c \"$*\"\n");
        const QString installs =
            nothing ? QString("echo nothing >> \"$LOG\"\nexit 1\n")
                    : QString("for package in \"$@\"; do\n"
                              "  case $package in\n"
                              "    speech-dispatcher) cp \"%1/spd-say.fake\" \"%1/spd-say\" ;;\n"
                              "    piper-tts-bin) cp \"%1/piper.fake\" \"%1/piper\" ;;\n"
                              "    piper-voices-*) mkdir -p \"%2/piper-voices\" && : > \"%2/piper-voices/en_US-fake-medium.onnx\" ;;\n"
                              "    voxtype-bin) cp \"%1/voxtype.fake\" \"%1/voxtype\" ;;\n"
                              "  esac\n"
                              "done\necho installed >> \"$LOG\"\n")
                          .arg(bin, share);
        script("omarchy-pkg-add", head + "printf 'pkg-add|%s\\n' \"$*\" >> \"$LOG\"\n" + installs);
        script("omarchy-pkg-aur-add", head + "printf 'aur-add|%s\\n' \"$*\" >> \"$LOG\"\n" + installs);
        // The voices, installed: each says what it was asked, and takes its time.
        script("spd-say.fake", head + "printf 'spd-say|%s\\n' \"$*\" >> \"$LOG\"\n[ \"$1\" = -S ] || exec sleep 30\n");
        script("piper.fake", head + "printf 'piper|%s\\n' \"$*\" >> \"$LOG\"\nexec sleep 30\n");
        script("voxtype.fake", head + "printf 'voxtype|%s\\n' \"$*\" >> \"$LOG\"\n");
    }
    ~Omarchy() {
        qputenv("PATH", path);
        dataDirs.isNull() ? qunsetenv("XDG_DATA_DIRS") : qputenv("XDG_DATA_DIRS", dataDirs);
    }
    Omarchy(const Omarchy &) = delete;
    Omarchy &operator=(const Omarchy &) = delete;

    void script(const QString &name, const QString &text) const {
        QFile file(bin + "/" + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(text.toUtf8());
        file.close();
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    [[nodiscard]] QStringList calls() const {
        QFile file(log);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
    }
    [[nodiscard]] bool called(const QString &line) const { return calls().contains(line); }
};

// The window with a book open, and nothing set to read it aloud but what is installed.
struct Bench {
    Desk desk;
    qint64 book = 0;

    explicit Bench(const QByteArray &language = "en", bool touch = false) : desk(language, {}, touch) {
        const QString path = desk.dir.path() + "/The Ferry.epub";
        QVERIFY(Fixtures::chapteredEpub(path, "The Ferry",
                                        {{"The Island", "<p>It was late in the autumn. The ferry came back to the island.</p>"}}));
        book = desk.backend->library()->add(path).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        desk.quicken();
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        desk.settle();
    }

    [[nodiscard]] ReadAloud *aloud() const { return desk.reader->readAloud(); }
    [[nodiscard]] bool offered() const {
        QObject *dialog = desk.find("voiceInstall");
        return dialog && dialog->property("opened").toBool();
    }
    void press(const QString &name) const { QMetaObject::invokeMethod(desk.find(name), "clicked"); }
    // The terminal takes the keyboard, and is closed again.
    void terminalClosed() const {
        QWindowSystemInterface::handleApplicationStateChanged<QWindowSystemInterface::SynchronousDelivery>(Qt::ApplicationInactive);
        QWindowSystemInterface::handleApplicationStateChanged<QWindowSystemInterface::SynchronousDelivery>(Qt::ApplicationActive);
    }
};

} // namespace

class VoiceInstallTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // The window's own style, as the app sets it.
        QQuickStyle::setStyle("Material");
        QStandardPaths::setTestModeEnabled(true);
    }

    // Piper's voice is the book's language's, of its region where the AUR has it, else its usual one, else English.
    void piperSpeaksTheBooksLanguage() {
        QCOMPARE(VoiceInstall::piperVoice(u"de"), QString("piper-voices-de-de"));
        QCOMPARE(VoiceInstall::piperVoice(u"nl-BE"), QString("piper-voices-nl-be"));
        QCOMPARE(VoiceInstall::piperVoice(u"nld"), QString("piper-voices-nl-nl"));
        QCOMPARE(VoiceInstall::piperVoice(u"es"), QString("piper-voices-es-es"));
        QCOMPARE(VoiceInstall::piperVoice(u"es_MX"), QString("piper-voices-es-mx"));
        QCOMPARE(VoiceInstall::piperVoice(u"pt"), QString("piper-voices-pt-br"));
        QCOMPARE(VoiceInstall::piperVoice(u"en-GB"), QString("piper-voices-en-gb"));
        QCOMPARE(VoiceInstall::piperVoice(u"fr-CA"), QString("piper-voices-fr-fr"));
        QCOMPARE(VoiceInstall::piperVoice(u"nb"), QString("piper-voices-no-no"));
        QCOMPARE(VoiceInstall::piperVoice(u"ja"), QString("piper-voices-en-us"));
        QCOMPARE(VoiceInstall::piperVoice(u""), QString("piper-voices-en-us"));
    }
    // What each installs, through which of Omarchy's commands.
    void eachIsInstalledTheWayOmarchyInstallsIt() {
        QCOMPARE(VoiceInstall::command(VoiceInstall::Kind::Voice, u"de"),
                 (VoiceInstall::Command{"omarchy-install-app", {"speech-dispatcher", "speech-dispatcher espeak-ng"}}));
        QCOMPARE(VoiceInstall::command(VoiceInstall::Kind::Piper, u"de"),
                 (VoiceInstall::Command{"omarchy-launch-floating-terminal-with-presentation",
                                        {"echo 'Installing Piper...'; omarchy-pkg-aur-add piper-tts-bin piper-voices-de-de"}}));
        QCOMPARE(VoiceInstall::command(VoiceInstall::Kind::Voxtype, u""),
                 (VoiceInstall::Command{"omarchy-launch-floating-terminal-with-presentation",
                                        {"echo 'Installing voxtype...'; omarchy-pkg-add voxtype-bin && voxtype setup --download "
                                         "--no-post-install"}}));
        const Omarchy elsewhere(false);
        QVERIFY(!VoiceInstall::offered());
        QVERIFY(!VoiceInstall::offered(VoiceInstall::Kind::Voxtype));
        const Omarchy omarchy;
        QVERIFY(VoiceInstall::offered());
        QVERIFY(VoiceInstall::offered(VoiceInstall::Kind::Piper));
        QVERIFY(VoiceInstall::offered(VoiceInstall::Kind::Voxtype));
    }

    // On Omarchy without voxtype, the AI page offers it; installed with its
    // model in the terminal, once that is closed it is what hears recordings,
    // said with what it doesn't give.
    void voxtypeIsOfferedInTheSettings() {
        const Omarchy omarchy;
        Desk desk("en", {}, false);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        SpeechInstall *speech = desk.backend->speech();
        QVERIFY(speech->installable());
        // In speech to text's row of the AI page (#144).
        desk.call("showAiSettings");
        QVERIFY(desk.waitForPopup());
        desk.find("aiSettings")->setProperty("current", QString("speech-to-text"));
        QTRY_VERIFY(desk.shows("speechInstall"));
        desk.shot("speech-install");
        QVERIFY(omarchy.calls().isEmpty());
        QMetaObject::invokeMethod(desk.find("speechInstall"), "clicked");
        QTRY_VERIFY(omarchy.called("installed"));
        QVERIFY(speech->installing());
        QVERIFY(!desk.shows("speechInstall"));
        QCOMPARE(
            omarchy.calls().first(),
            QString("floating|echo 'Installing voxtype...'; omarchy-pkg-add voxtype-bin && voxtype setup --download --no-post-install"));
        QTRY_VERIFY(omarchy.called("voxtype|setup --download --no-post-install"));
        QWindowSystemInterface::handleApplicationStateChanged<QWindowSystemInterface::SynchronousDelivery>(Qt::ApplicationInactive);
        QWindowSystemInterface::handleApplicationStateChanged<QWindowSystemInterface::SynchronousDelivery>(Qt::ApplicationActive);
        QTRY_VERIFY(!speech->installing());
        QVERIFY(!speech->installable());
        QCOMPARE(speech->heardBy(), QString("voxtype on this computer"));
        QVERIFY(!speech->timed());
        QTRY_VERIFY(desk.find("speechHeardBy")->property("text").toString().contains("doesn't give each word its moment"));
        QVERIFY(!desk.shows("speechInstall"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
    // Not on Omarchy, nothing is offered.
    void voxtypeIsNotOfferedElsewhere() {
        const Omarchy elsewhere(false);
        Desk desk("en", {}, false);
        QVERIFY(!desk.backend->speech()->installable());
        desk.backend->speech()->install();
        QVERIFY(!desk.backend->speech()->installing());
    }

    // R with no voice offers one; chosen, it is installed in the terminal, and once that is closed the book is read.
    void aVoiceIsInstalledAndTheBookRead() {
        const Omarchy omarchy;
        Bench bench;
        QTest::keyClick(bench.desk.window, Qt::Key_R);
        QTRY_VERIFY(bench.offered());
        bench.desk.shot("voice-install");
        // Nothing ran before the choice.
        QVERIFY(omarchy.calls().isEmpty());
        QVERIFY(!bench.aloud()->active());
        bench.press("voiceInstallVoice");
        QTRY_VERIFY(!bench.offered());
        QVERIFY(bench.aloud()->installing());
        QTRY_VERIFY(omarchy.called("installed"));
        QCOMPARE(omarchy.calls().first(), QString("install-app|speech-dispatcher|speech-dispatcher espeak-ng"));
        // Installed, while its terminal is still open: not yet.
        QTest::qWait(100);
        QVERIFY(!bench.aloud()->active());
        bench.terminalClosed();
        QTRY_VERIFY(bench.aloud()->active());
        QVERIFY(!bench.aloud()->installing());
        QCOMPARE(bench.aloud()->engine(), QString("speech-dispatcher"));
        // Read by what was installed, in the book's language.
        QTRY_VERIFY(!omarchy.calls().filter("spd-say|-w -r 0 -l en -- ").isEmpty());
        bench.aloud()->stop();
        // The voice's command, stopped, has ended before the window goes.
        QTest::qWait(200);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }
    void piperIsInstalledFromTheAurWithTheBooksVoice() {
        const Omarchy omarchy;
        Bench bench;
        QTest::keyClick(bench.desk.window, Qt::Key_R);
        QTRY_VERIFY(bench.offered());
        QVERIFY(bench.desk.shows("voiceInstallPiper"));
        QVERIFY(bench.desk.find("voiceInstallPiperText")->property("text").toString().contains("piper-voices-en-us"));
        bench.press("voiceInstallPiper");
        QTRY_VERIFY(omarchy.called("installed"));
        QCOMPARE(omarchy.calls().first(),
                 QString("floating|echo 'Installing Piper...'; omarchy-pkg-aur-add piper-tts-bin piper-voices-en-us"));
        bench.terminalClosed();
        QTRY_VERIFY(bench.aloud()->active());
        QCOMPARE(bench.aloud()->engine(), QString("Piper"));
        QTRY_VERIFY(omarchy.calls().filter("piper|-m ").size() > 0);
        QVERIFY(omarchy.calls().filter("piper|-m ").first().contains("en_US-fake-medium.onnx"));
        bench.aloud()->stop();
        // The voice's command, stopped, has ended before the window goes.
        QTest::qWait(200);
    }
    // The terminal closed with nothing installed: it is said, as it was before.
    void noVoiceCameAndThatIsSaid() {
        const Omarchy omarchy(true, true);
        Bench bench;
        QTest::keyClick(bench.desk.window, Qt::Key_R);
        QTRY_VERIFY(bench.offered());
        bench.press("voiceInstallVoice");
        QTRY_VERIFY(omarchy.called("nothing"));
        bench.terminalClosed();
        QTRY_VERIFY(!bench.aloud()->installing());
        QVERIFY(!bench.aloud()->active());
        QCOMPARE(bench.desk.backend->message(), Failure(Failure::Code::NoVoice).text());
    }
    // Cancelled, nothing runs, and R offers it again.
    void cancelledNothingIsInstalled() {
        const Omarchy omarchy;
        Bench bench;
        QTest::keyClick(bench.desk.window, Qt::Key_R);
        QTRY_VERIFY(bench.offered());
        QTest::keyClick(bench.desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!bench.offered());
        QVERIFY(!bench.aloud()->offering());
        QTest::qWait(100);
        QVERIFY(omarchy.calls().isEmpty());
        QTest::keyClick(bench.desk.window, Qt::Key_R);
        QTRY_VERIFY(bench.offered());
        bench.press("voiceInstallCancel");
        QTRY_VERIFY(!bench.offered());
        QVERIFY(omarchy.calls().isEmpty());
    }
    // Not on Omarchy: what to install is said, and nothing offered.
    void elsewhereItIsOnlySaid() {
        const Omarchy elsewhere(false);
        Bench bench;
        QTest::keyClick(bench.desk.window, Qt::Key_R);
        QTRY_COMPARE(bench.desk.backend->message(), Failure(Failure::Code::NoVoice).text());
        QVERIFY(!bench.offered());
        QVERIFY(!bench.aloud()->installable());
    }
    // The Aa panel offers it too; installed from there, the voice is shown, and nothing read.
    void theAaPanelOffersItToo() {
        const Omarchy omarchy;
        Bench bench;
        QObject *view = bench.desk.find("readerView");
        QMetaObject::invokeMethod(view, "toggleFormat");
        QTRY_VERIFY(bench.desk.shows("readAloudInstall"));
        bench.desk.shot("voice-install-panel");
        bench.press("readAloudInstall");
        QTRY_VERIFY(bench.offered());
        bench.press("voiceInstallVoice");
        QTRY_VERIFY(omarchy.called("installed"));
        bench.terminalClosed();
        QTRY_VERIFY(!bench.aloud()->installing());
        QCOMPARE(bench.aloud()->engine(), QString("speech-dispatcher"));
        QVERIFY(!bench.aloud()->active());
        QTRY_VERIFY(!bench.desk.shows("readAloudInstall"));
    }

    // The offer, at the window's sizes, in a language of long words and one written right to left, and under fingers.
    void theOfferIsLaidOut_data() {
        QTest::addColumn<QByteArray>("language");
        QTest::addColumn<bool>("touch");
        QTest::newRow("en") << QByteArray("en") << false;
        QTest::newRow("de") << QByteArray("de") << false;
        QTest::newRow("ar") << QByteArray("ar") << false;
        QTest::newRow("de touch") << QByteArray("de") << true;
    }
    void theOfferIsLaidOut() {
        QFETCH(QByteArray, language);
        QFETCH(bool, touch);
        const Omarchy omarchy;
        Bench bench(language, touch);
        QStringList problems;
        for (const QSize &size : dialogSizes()) {
            QVERIFY(bench.desk.resize(size));
            bench.aloud()->offerInstall();
            QTRY_VERIFY(bench.offered());
            QVERIFY(bench.desk.waitForPopup());
            Survey survey{bench.desk.window, touch, {}};
            survey.walk();
            const QString where =
                QString("%1%2 %3x%4").arg(QString::fromLatin1(language), touch ? " touch" : "").arg(size.width()).arg(size.height());
            for (const QString &problem : std::as_const(survey.problems))
                problems << where + " — " + problem;
            bench.desk.shot(QString("voice-install-%1%2-%3x%4")
                                .arg(QString::fromLatin1(language), touch ? "-touch" : "")
                                .arg(size.width())
                                .arg(size.height()));
            QVERIFY(bench.desk.closePopups());
        }
        QVERIFY(omarchy.calls().isEmpty());
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }
};

QTEST_MAIN(VoiceInstallTests)
#include "voiceinstall_tests.moc"
