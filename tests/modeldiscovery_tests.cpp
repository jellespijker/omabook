// Finding new models on Ollama's library and Hugging Face (#175): against a
// fake Ollama site and registry, a fake Hub and a fake Ollama server, the
// search for each capability (by its category and task, sending nothing but
// the words searched for), the size and the memory each needs marked as
// fitting the computer or not, the licence, a hosted model held to the hosted
// rule, installing onto Ollama and choosing it, speech downloaded where
// whisper.cpp and Piper look, and nothing searched with the AI Off.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>

#include "aiprivacy.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "modeldiscovery.h"
#include "modelsearch.h"
#include "secretstore.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

constexpr qint64 gigabyte = 1024LL * 1024 * 1024;

// Two models as ollama.com/search shows them, marked for its own tests.
constexpr auto libraryPage = R"(<html><body><ul>
<li x-test-model class="flex">
  <a href="/library/qwen2.5vl" class="group w-full">
    <h2><span x-test-search-response-title>qwen2.5vl</span></h2>
    <p class="max-w-lg break-words">Qwen&#39;s flagship vision-language model.</p>
    <span x-test-capability class="tag">vision</span>
    <span x-test-size class="tag">3b</span>
    <span x-test-size class="tag">72b</span>
    <p class="my-1"><span x-test-pull-count>1.2M</span> Pulls</p>
  </a>
</li>
<li x-test-model class="flex">
  <a href="/library/qwen3-vl" class="group w-full">
    <h2><span x-test-search-response-title>qwen3-vl</span></h2>
    <p class="max-w-lg">Runs on Ollama's computers.</p>
    <span x-test-capability>vision</span><span x-test-capability>cloud</span>
    <p class="my-1"><span x-test-pull-count>56K</span></p>
  </a>
</li>
</ul></body></html>)";

[[nodiscard]] FakeServer::Reply json(const QJsonValue &body) {
    const QByteArray bytes = body.isArray() ? QJsonDocument(body.toArray()).toJson(QJsonDocument::Compact)
                                            : QJsonDocument(body.toObject()).toJson(QJsonDocument::Compact);
    return {.status = "200 OK", .body = bytes, .type = "application/json"};
}

[[nodiscard]] QJsonObject file(const QString &path, qint64 size) { return {{"type", "file"}, {"path", path}, {"size", size}}; }

// Ollama's site, its registry and the Hub, on one fake server; and an Ollama server to install onto.
struct Sites {
    FakeServer site;
    FakeServer ollama;
    QStringList pulled;

    Sites() {
        site.answering = [this](const FakeServer::Request &request) { return answerSite(request); };
        ollama.answering = [this](const FakeServer::Request &request) { return answerOllama(request); };
    }
    [[nodiscard]] QString root() const { return u"http://127.0.0.1:%1"_s.arg(site.serverPort()); }
    [[nodiscard]] ModelSearch::Sites sites() const { return {.library = root(), .registry = root(), .hub = root()}; }
    [[nodiscard]] QStringList paths() const {
        QStringList all;
        for (const auto &request : site.received)
            all << QString::fromUtf8(request.line.split(' ').value(1));
        return all;
    }

    FakeServer::Reply answerSite(const FakeServer::Request &request) const {
        const QString path = QString::fromUtf8(request.line.split(' ').value(1));
        if (path.startsWith("/search"_L1))
            return {.status = "200 OK", .body = libraryPage, .type = "text/html"};
        if (path == "/v2/library/qwen2.5vl/manifests/3b"_L1)
            return json(QJsonObject{
                {"config", QJsonObject{{"size", 1000}}},
                {"layers",
                 QJsonArray{QJsonObject{{"mediaType", "application/vnd.ollama.image.model"}, {"size", 3 * gigabyte}},
                            QJsonObject{{"mediaType", "application/vnd.ollama.image.license"}, {"digest", "sha256:abc"}, {"size", 30}}}}});
        if (path == "/v2/library/qwen2.5vl/blobs/sha256:abc"_L1)
            return {.status = "200 OK", .body = "\n  Apache License\n  Version 2.0, January 2004\n", .type = "text/plain"};
        if (path.startsWith("/api/models?"_L1))
            return json(QJsonArray{
                QJsonObject{
                    {"id", "someone/Qwen-VL-GGUF"}, {"downloads", 4200}, {"likes", 17}, {"tags", QJsonArray{"gguf", "license:apache-2.0"}}},
                QJsonObject{{"id", "someone/No-GGUF"}, {"downloads", 10}, {"tags", QJsonArray{"license:mit"}}}});
        if (path == "/api/models/someone/Qwen-VL-GGUF/tree/main"_L1)
            return json(QJsonArray{file(u"Qwen-VL.Q8_0.gguf"_s, 8 * gigabyte), file(u"Qwen-VL.Q4_K_M.gguf"_s, 4 * gigabyte),
                                   file(u"mmproj-Qwen-VL-f16.gguf"_s, gigabyte / 2), file(u"README.md"_s, 100)});
        if (path == "/api/models/someone/No-GGUF/tree/main"_L1)
            return json(QJsonArray{file(u"model.safetensors"_s, 100)});
        if (path.startsWith("/api/models/ggerganov/whisper.cpp/tree/main"_L1))
            return json(QJsonArray{file(u"ggml-base.en.bin"_s, 5), file(u"ggml-large-v3.bin"_s, 9), file(u"README.md"_s, 1)});
        if (path.startsWith("/api/models/rhasspy/piper-voices/tree/main"_L1))
            return json(QJsonArray{file(u"en/en_US/amy/medium/en_US-amy-medium.onnx"_s, 7),
                                   file(u"en/en_US/amy/medium/en_US-amy-medium.onnx.json"_s, 2),
                                   file(u"de/de_DE/thorsten/high/de_DE-thorsten-high.onnx"_s, 8)});
        if (path == "/api/models/ggerganov/whisper.cpp"_L1 || path == "/api/models/rhasspy/piper-voices"_L1)
            return json(QJsonObject{{"id", "x"}, {"tags", QJsonArray{"license:mit"}}});
        if (path.contains("/resolve/main/"_L1))
            return {.status = "200 OK", .body = "weights of " + path.toUtf8(), .type = "application/octet-stream"};
        return {.status = "404 Not Found", .body = "{}", .type = "application/json"};
    }
    FakeServer::Reply answerOllama(const FakeServer::Request &request) {
        if (!request.line.contains("/api/pull"))
            return {.status = "404 Not Found", .body = "{}", .type = "application/json"};
        pulled << request.json().value("model").toString();
        return {.status = "200 OK",
                .body = R"({"status":"pulling 970aa74c","total":1000,"completed":1000})"
                        "\n"
                        R"({"status":"success"})"
                        "\n",
                .type = "application/x-ndjson"};
    }
};

struct Bench {
    QTemporaryDir dir;
    Settings settings{dir.filePath("config.json")};
    Bench() { AiPrivacy::carryOver(settings); }
};

[[nodiscard]] bool settled(ModelSearch &search) {
    return QTest::qWaitFor([&search] { return search.state() != u"searching"_s && search.state() != u"installing"_s; }, 10000);
}

[[nodiscard]] QVariantMap named(const ModelSearch &search, const QString &name) {
    for (const QVariant &one : search.results())
        if (one.toMap().value("name").toString() == name)
            return one.toMap();
    return {};
}

} // namespace

class ModelDiscoveryTests : public QObject {
    Q_OBJECT

    SessionSecrets m_secrets;

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        Secrets::useInstead(&m_secrets);
    }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }

    // Ollama's search page: a result for each size, the hosted one marked, its pulls counted.
    void ollamasSearchPageIsReadBySize() {
        const QList<Discovery::Found> found = Discovery::fromLibrary(QString::fromLatin1(libraryPage));
        QCOMPARE(found.size(), 3);
        QCOMPARE(found.at(0).name, u"qwen2.5vl:3b"_s);
        QCOMPARE(found.at(0).description, u"Qwen's flagship vision-language model."_s);
        QCOMPARE(found.at(0).downloads, 1200000);
        QCOMPARE(found.at(1).name, u"qwen2.5vl:72b"_s);
        QVERIFY(found.at(1).bytes > found.at(0).bytes);
        QCOMPARE(found.at(2).name, u"qwen3-vl:cloud"_s);
        QVERIFY(found.at(2).hosted);
        // Q4_K_M, some 4.85 bits a weight.
        QCOMPARE(Discovery::bytesOfParameters(u"3b"_s), 1818750000);
        QCOMPARE(Discovery::bytesOfParameters(u"300m"_s), 181875000);
        QCOMPARE(Discovery::bytesOfParameters(u"latest"_s), 0);
    }

    // The memory free here: the GPU's first, then the computer's, else too big.
    void fitIsJudgedByTheFreeMemory() {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath("proc"));
        QVERIFY(QDir(root.path()).mkpath("sys/class/drm/card0/device"));
        QVERIFY(QDir(root.path()).mkpath("sys/class/drm/card0-DP-1"));
        QVERIFY(Fixtures::write(root.filePath("proc/meminfo"), "MemTotal: 32000000 kB\nMemAvailable:    8388608 kB\n"));
        QVERIFY(Fixtures::write(root.filePath("sys/class/drm/card0/device/mem_info_vram_total"), QByteArray::number(4 * gigabyte)));
        QVERIFY(Fixtures::write(root.filePath("sys/class/drm/card0/device/mem_info_vram_used"), QByteArray::number(gigabyte)));
        const Discovery::Free free = Discovery::here(root.path());
        QVERIFY(free.known);
        QCOMPARE(free.memory, 8 * gigabyte);
        QCOMPARE(free.gpu, 3 * gigabyte);
        QCOMPARE(Discovery::fit(2 * gigabyte, free), Discovery::Fit::Gpu);
        QCOMPARE(Discovery::fit(6 * gigabyte, free), Discovery::Fit::Memory);
        QCOMPARE(Discovery::fit(20 * gigabyte, free), Discovery::Fit::TooBig);
        QCOMPARE(Discovery::fit(2 * gigabyte, {}), Discovery::Fit::Unknown);
        QVERIFY(Discovery::needs(Capability::Answers, 2 * gigabyte) > 2 * gigabyte);
    }

    // Each capability is searched for by its kind; only the words go out.
    void aSearchFindsByCapabilityAndSendsOnlyTheWords_data() {
        QTest::addColumn<QString>("capability");
        QTest::addColumn<QString>("category");
        QTest::addColumn<QString>("task");
        QTest::newRow("handwriting") << u"handwriting"_s << u"vision"_s << u"image-text-to-text"_s;
        QTest::newRow("embeddings") << u"embeddings"_s << u"embedding"_s << u"feature-extraction"_s;
        QTest::newRow("answers") << u"answers"_s << QString() << u"text-generation"_s;
    }
    void aSearchFindsByCapabilityAndSendsOnlyTheWords() {
        QFETCH(QString, capability);
        QFETCH(QString, category);
        QFETCH(QString, task);
        Sites sites;
        Bench bench;
        bench.settings.setAiProvider("openai");
        bench.settings.setAiEndpoint({{"preset", "custom"}, {"base_url", sites.ollama.url()}});
        ModelSearch search(&bench.settings, *Capabilities::named(capability));
        search.useSites(sites.sites());
        search.useFree([] { return Discovery::Free{.memory = 8 * gigabyte, .gpu = 2 * gigabyte, .known = true}; });
        search.search(u"qwen"_s);
        QVERIFY(settled(search));
        QCOMPARE(search.state(), u"found"_s);

        const QStringList paths = sites.paths();
        const auto searched = std::ranges::find_if(paths, [](const QString &path) { return path.startsWith("/search"_L1); });
        QVERIFY(searched != paths.end());
        const QUrlQuery library(QUrl(*searched).query());
        QCOMPARE(library.queryItemValue(u"q"_s), u"qwen"_s);
        QCOMPARE(library.queryItemValue(u"c"_s), category);
        const auto hub = std::ranges::find_if(paths, [](const QString &path) { return path.startsWith("/api/models?"_L1); });
        QVERIFY(hub != paths.end());
        const QUrlQuery asked(QUrl(*hub).query());
        QCOMPARE(asked.queryItemValue(u"pipeline_tag"_s), task);
        QCOMPARE(asked.queryItemValue(u"filter"_s), u"gguf"_s);
        // Nothing but GETs of those words: no body, nothing of the person's.
        for (const auto &request : sites.site.received) {
            QVERIFY(request.line.startsWith("GET "));
            QVERIFY(request.body.isEmpty());
        }
        QVERIFY(sites.ollama.received.isEmpty());

        // Its size from its manifest, its licence, and it fits the GPU; the larger only the memory, or not at all.
        const QVariantMap small = named(search, u"qwen2.5vl:3b"_s);
        QCOMPARE(small.value("bytes").toLongLong(), 3 * gigabyte + 1000 + 30); // every layer, its licence too
        QCOMPARE(small.value("licence").toString(), u"Apache License"_s);
        QCOMPARE(small.value("fit").toString(), u"memory"_s);
        QCOMPARE(named(search, u"qwen2.5vl:72b"_s).value("fit").toString(), u"too-big"_s);
        QCOMPARE(named(search, u"qwen3-vl:cloud"_s).value("hosted").toBool(), true);
        // The Hub's GGUF, as Ollama pulls it, with its vision projector; one without GGUF is left out.
        const QVariantMap gguf = named(search, u"hf.co/someone/Qwen-VL-GGUF:Q4_K_M"_s);
        QCOMPARE(gguf.value("bytes").toLongLong(), 4 * gigabyte + gigabyte / 2);
        QCOMPARE(gguf.value("licence").toString(), u"apache-2.0"_s);
        QCOMPARE(gguf.value("source").toString(), u"hugging-face"_s);
        QVERIFY(named(search, u"hf.co/someone/No-GGUF"_s).isEmpty());
    }

    // Installed onto the capability's Ollama server with one press, then chosen;
    // a hosted one only where hosted is allowed.
    void aModelIsInstalledAndChosen() {
        Sites sites;
        Bench bench;
        bench.settings.setRecognition({{"ink_url", sites.ollama.url()}, {"ink_model", "llava"}});
        ModelSearch search(&bench.settings, Capability::Handwriting);
        search.useSites(sites.sites());
        search.search(u"qwen"_s);
        QVERIFY(settled(search));
        QVERIFY(search.installable());

        const int cloud = static_cast<int>(search.results().indexOf(named(search, u"qwen3-vl:cloud"_s)));
        search.install(cloud);
        QCOMPARE(search.state(), u"failed"_s);
        QVERIFY(sites.pulled.isEmpty());

        QSignalSpy chosen(&search, &ModelSearch::chosen);
        search.install(static_cast<int>(search.results().indexOf(named(search, u"qwen2.5vl:3b"_s))));
        QVERIFY(settled(search));
        QCOMPARE(search.state(), u"chosen"_s);
        QCOMPARE(sites.pulled, QStringList{u"qwen2.5vl:3b"_s});
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(bench.settings.recognition().value("ink_model").toString(), u"qwen2.5vl:3b"_s);
    }

    // Speech either way: a file of whisper.cpp's or Piper's, where each looks, then chosen.
    void speechIsDownloadedWhereItsEngineLooks() {
        Sites sites;
        Bench bench;
        ModelSearch heard(&bench.settings, Capability::SpeechToText);
        heard.useSites(sites.sites());
        heard.search(u"base"_s);
        QVERIFY(settled(heard));
        QCOMPARE(heard.results().size(), 1);
        QCOMPARE(heard.results().first().toMap().value("licence").toString(), u"mit"_s);
        heard.install(0);
        QVERIFY(settled(heard));
        QCOMPARE(heard.state(), u"chosen"_s);
        const QString model =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/whisper.cpp/models/ggml-base.en.bin"_s;
        QVERIFY(QFile::exists(model));
        QCOMPARE(bench.settings.recognition().value("whisper_model").toString(), model);
        QCOMPARE(AiPrivacy::chosen(bench.settings, Capability::SpeechToText).value("provider").toString(), u"whisper.cpp"_s);

        ModelSearch read(&bench.settings, Capability::TextToSpeech);
        read.useSites(sites.sites());
        read.search(u"en_US"_s);
        QVERIFY(settled(read));
        QCOMPARE(read.results().size(), 1);
        read.install(0);
        QVERIFY(settled(read));
        QCOMPARE(read.state(), u"chosen"_s);
        const QString voices = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/piper-voices/"_s;
        QVERIFY(QFile::exists(voices + u"en_US-amy-medium.onnx"_s));
        QVERIFY(QFile::exists(voices + u"en_US-amy-medium.onnx.json"_s));
        QCOMPARE(bench.settings.readAloud().value("engine").toString(), u"piper"_s);
        QFile::remove(model);
        QDir(voices).removeRecursively();
    }

    // Off is off: nothing is searched.
    void withTheAiOffNothingIsSearched() {
        Sites sites;
        Bench bench;
        bench.settings.setAiProvider("off");
        ModelSearch search(&bench.settings, Capability::Embeddings);
        search.useSites(sites.sites());
        search.search(u"nomic"_s);
        QCOMPARE(search.state(), u"off"_s);
        QTest::qWait(100);
        QVERIFY(sites.site.received.isEmpty());
    }
};

QTEST_MAIN(ModelDiscoveryTests)
#include "modeldiscovery_tests.moc"
