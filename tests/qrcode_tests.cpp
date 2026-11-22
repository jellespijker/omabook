// A QR code of an address, as the Phones and e-readers panel shows one (#177):
// the same codewords in the same places as another encoder (libqrencode,
// level M, byte mode) makes of the same text, version information included
// (each chooses its mask by its own reading of the penalties), and read back
// by a reader written apart from it (qrread.h) at every version it makes,
// its blocks checked by their syndromes. More than version 10 holds is no code.

#include <QImage>
#include <QPainter>
#include <QtTest>

#include "qrcode.h"
#include "qrread.h"

namespace {

// libqrencode's codes of two addresses, "#" dark: version 3, and version 8,
// which has the version information and several blocks of two lengths.
const QStringList shortCode{
    "#######.#.#...#.#.#...#######", "#.....#.###........##.#.....#", "#.###.#.#...###.#.##..#.###.#", "#.###.#..#.###...#....#.###.#",
    "#.###.#.###..##..###..#.###.#", "#.....#..###.##.##..#.#.....#", "#######.#.#.#.#.#.#.#.#######", "...........#.#.#...#.........",
    "#..######.###.##.##..#..#.###", "..##...#.#.##..#####.#.##.##.", ".#.#.####..##.##.....#..#.#..", "..##.#...#..#..#.##.....#...#",
    "..##.########..#...#####....#", ".##..#.##....###......####.##", "...#.##......####..#....###.#", "##.##..#..#.#.##..#.#####.#.#",
    "##....#....###.##..#...#.....", "#....#..###...####.###..####.", "####..#.##....###.##.##.#...#", "##.###.#.#####.#...#....###..",
    "#####.##..##.#...###########.", "........#.#.#######.#...##...", "#######.#...##.#...##.#.##...", "#.....#.###..#...####...##.##",
    "#.###.#.##.###.#...######..#.", "#.###.#.####.#######.#...##.#", "#.###.#.....####.#..##.######", "#.....#...#.#.#...#...#####.#",
    "#######.##.##.##....#.#.#....",
};
const QStringList longCode{
    "#######..#.#.###.#.#.#...#####..#..#....#.#######", "#.....#..##.#..#.####.#.....#.###########.#.....#",
    "#.###.#.#..########.....#####.####.....##.#.###.#", "#.###.#.##..#...##...###..#...##..####.#..#.###.#",
    "#.###.#.#####..#.###########.#####.###....#.###.#", "#.....#.##.###..#.#.#.#...###..####.#.#...#.....#",
    "#######.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#######", "........#.##.##..####.#...#.#####....#.#.........",
    "#.#####...###.#.###.#.#####...##.####..##.#####..", "..##.#..#.###.##..##.#...#..####...###.#.###.#...",
    "...#..#.####.##.#.#....#..#.#..#####..#.##.#...##", ".##.#..#.#....#####.#......##.###.#..#..##..#..#.",
    ".#.####.#....##.#..#..###..#...#.#.##..###...###.", ".#.#.#.######..#..#..#.###..#.##....#.....###.##.",
    "..#..##.##...###.###...#..##.#.####.#####...#..##", "#.##.....##.##.##.#..#...##...##..##.##.##..#...#",
    "#..##.#...##.#..#.#.#..#####.##...####..###...#..", ".#...#...#.##...####...#.#....##...#.#...###.....",
    ".#..###.#.###..###...###..##.##...#.###..#.#.#..#", ".#...#..###.#....###.#.#...###..#.#..#..###.#...#",
    ".##...###.#.#.#.#...#.#.##....##.####..##.....###", "#..#...#..#.##...###.#..#..#.##.#...##...##.#.#..",
    "#..##########..#..##..######...#.####.#.#####..##", "#.###...#...#.#..#.####...#####.#.#..####...#..##",
    "##..#.#.#...#.##..#.###.#.#..##...#.###.#.#.####.", "....#...##.####..##...#...#.####.....#.##...##...",
    ".#..#######.#...###.#.#####.##....##..#########.#", "....##.#...##.#####..#.###..##..#.##.##.#...#...#",
    ".#...#####..#.##.#...#.####..###..####.#..#.#####", ".##.##.#.##.#..#######..#.##.###...###..##.#.....",
    "##..#.##..#.#...###.#.####.......###..#...####.##", ".#..##.#.####...#.###.#.#.##.#...#.#....#...#..##",
    "..#..##..#.#.###..###.#..#..#..######..###..#####", "....#..##..####..###....#..###..#..#.#.#.#.#...#.",
    "...#..##......#....#.#####..#.#####.#.#..#####.##", "....##.#....#..##..###.####.###.#..#.#...#.##..##",
    "....#.##..#.#...#.###.##.##..###...###.#.##.#.#.#", "#....#.######.#.###..#..#.#...#.#..###.#.###.#...",
    ".#...##.####...#.###...#.#..#..#..#.####.##....##", ".###....##..#......##.#.#.###.#.##....#.##.#...#.",
    "###...#..####..#...##.######.#.#.###############.", "........####..#..##...#...##.###.#.###.##...#.##.",
    "#######...###.....#.#.#.#.#..#.######.#.#.#.#.###", "#.....#.#..#.##.##...##...#.#.#.#......##...#..#.",
    "#.###.#.##.#.##..#.#..#####..###..#.############.", "#.###.#.#.....##..#..###.####.#..#...#.##..###..#",
    "#.###.#.#...##.#..##.#.##.#.#...###.#.#..#.......", "#.....#..#.#...###...#.####...#....#.....##.....#",
    "#######.##.......#..##.##..#.##..#.###..#....####",
};

QrRead::Modules modulesOf(const QStringList &rows) {
    QrRead::Modules modules;
    for (const QString &row : rows) {
        std::vector<bool> line;
        for (const QChar module : row)
            line.push_back(module == '#');
        modules.push_back(line);
    }
    return modules;
}

QStringList rows(const QrCode &code) {
    QStringList result;
    for (int y = 0; y < code.size(); ++y) {
        QString row;
        for (int x = 0; x < code.size(); ++x)
            row += code.dark(x, y) ? '#' : '.';
        result << row;
    }
    return result;
}

// The code drawn as the panel draws it: whole pixels a module, on a light margin.
QImage picture(const QrCode &code, int module) {
    const int side = (code.size() + 8) * module;
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    for (int y = 0; y < code.size(); ++y)
        for (int x = 0; x < code.size(); ++x)
            if (code.dark(x, y))
                painter.fillRect((x + 4) * module, (y + 4) * module, module, module, Qt::black);
    return image;
}

} // namespace

class QrCodeTests : public QObject {
    Q_OBJECT

private slots:
    void itIsTheCodeAnotherEncoderMakes() {
        const auto small = QrCode::of("http://reader-pc.example.ts.net:6622/opds");
        QVERIFY(small);
        QCOMPARE(small->version(), 3);
        // The same codewords in the same places; the mask each chose by its own reading of the penalties.
        QCOMPARE(QrRead::decode(modulesOf(shortCode)), std::optional<QByteArray>("http://reader-pc.example.ts.net:6622/opds"));
        QCOMPARE(QrRead::dataBits(modulesOf(rows(*small))), QrRead::dataBits(modulesOf(shortCode)));
        const auto large = QrCode::of("http://reader-pc.example.ts.net:6622/t/made-up-token-for-a-test/opds?from=omabook&kind=catalog&for="
                                      "a-phone-app-that-takes-no-password");
        QVERIFY(large);
        QCOMPARE(large->version(), 8);
        QVERIFY(QrRead::decode(modulesOf(longCode)));
        QCOMPARE(QrRead::dataBits(modulesOf(rows(*large))), QrRead::dataBits(modulesOf(longCode)));
    }

    void itReadsBackAtEveryVersion() {
        // The most each version holds at level M, and one byte more.
        const QList<int> most{14, 26, 42, 62, 84, 106, 122, 152, 180, 213};
        for (qsizetype at = 0; at < most.size(); ++at) {
            const int version = static_cast<int>(at) + 1;
            for (const int length : {most.at(at), most.at(at) + 1}) {
                QByteArray text;
                for (int i = 0; i < length; ++i)
                    text += "http://reader-pc.example.ts.net:6622/t/AbCd23/opds"[i % 50];
                const auto code = QrCode::of(text);
                if (version == 10 && length > most.at(at)) {
                    QVERIFY2(!code, "more than version 10 holds");
                    continue;
                }
                QVERIFY(code);
                QCOMPARE(code->version(), length > most.at(at) ? version + 1 : version);
                QCOMPARE(code->size(), 17 + 4 * code->version());
                QCOMPARE(QrRead::read(picture(*code, 3)), std::optional<QByteArray>(text));
            }
        }
    }

    void outsideItIsLight() {
        const auto code = QrCode::of("x");
        QVERIFY(code);
        QVERIFY(!code->dark(-1, 0));
        QVERIFY(!code->dark(0, code->size()));
        QVERIFY(code->dark(0, 0)); // the top left finder's corner
    }
};

QTEST_MAIN(QrCodeTests)
#include "qrcode_tests.moc"
