#include "modelsearch.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

#include "aiprivacy.h"
#include "failure.h"
#include "recognition.h"
#include "settings.h"
#include "voices.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

// How many of each site's results are weighed (their files asked for their size).
constexpr qsizetype mostWeighed = 8;

[[nodiscard]] QString hostOf(const QString &url) { return QUrl(url).host(); }

// Where whisper.cpp and Piper look for what they read with (WhisperLocal::findModel, PiperVoice::findModels).
[[nodiscard]] QString speechFolder(Capability capability) {
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return capability == Capability::SpeechToText ? data + u"/whisper.cpp/models"_s : data + u"/piper-voices"_s;
}

// What each needs to run and whether that fits, and a speech file its repository's licence.
void judge(QList<Discovery::Found> &found, Capability capability, const Discovery::Free &free, const QString &licence) {
    for (Discovery::Found &one : found) {
        if (one.licence.isEmpty())
            one.licence = licence;
        one.needs = one.hosted ? 0 : Discovery::needs(capability, one.bytes);
        one.fit = Discovery::fit(one.needs, free);
    }
}

} // namespace

ModelSearch::ModelSearch(QObject *parent) : QObject(parent), m_freeHere([] { return Discovery::here(); }) {}

ModelSearch::ModelSearch(Settings *settings, Capability capability, QObject *parent) : ModelSearch(parent) {
    m_settings = settings;
    m_capability = capability;
}

ModelSearch::~ModelSearch() { cancel(); }

QObject *ModelSearch::settings() const { return m_settings.data(); }

void ModelSearch::setSettings(QObject *settings) {
    m_settings = qobject_cast<Settings *>(settings);
    emit settingsChanged();
}

QString ModelSearch::capability() const { return m_capability ? Capabilities::name(kind()) : QString(); }

void ModelSearch::setCapability(const QString &name) {
    m_capability = Capabilities::named(name);
    emit settingsChanged();
}

QVariantList ModelSearch::results() const {
    QVariantList all;
    for (const auto *found : {&m_library, &m_hub})
        for (const Discovery::Found &one : *found) {
            QVariantMap shown = one.toMap();
            // The sizes as the window says them, in its language.
            shown.insert(u"size"_s, one.bytes > 0 ? QLocale().formattedDataSize(one.bytes) : QString());
            shown.insert(u"needed"_s, one.needs > 0 ? QLocale().formattedDataSize(one.needs) : QString());
            all.append(shown);
        }
    return all;
}

Capability ModelSearch::kind() const { return m_capability.value_or(Capability::Answers); }

QString ModelSearch::where() const { return m_place ? m_place->server.host() : QString(); }

bool ModelSearch::installable() const { return m_capability && (!Discovery::pulled(kind()) || m_place); }

void ModelSearch::setState(const QString &state) {
    m_state = state;
    emit changed();
}

void ModelSearch::get(const QString &url, int round, const std::function<void(int status, const QByteArray &body)> &then) {
    QNetworkRequest request{QUrl(url)};
    request.setTransferTimeout(30s);
    // A registry answers its manifest by what is accepted; the rest take anything.
    request.setRawHeader("Accept", "application/vnd.docker.distribution.manifest.v2+json, application/json;q=0.9, */*;q=0.5");
    request.setHeader(QNetworkRequest::UserAgentHeader, u"Omabook"_s);
    ++m_pending;
    QNetworkReply *reply = m_network.get(request);
    m_replies.append(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, round, then, url] {
        reply->deleteLater();
        m_replies.removeAll(reply);
        if (round != m_round)
            return;
        --m_pending;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 && m_failed.isEmpty())
            m_failed =
                Failure(Failure::Code::ModelSearchUnanswered, {hostOf(url), status == 0 ? reply->errorString() : QString::number(status)})
                    .text();
        then(status, reply->readAll());
        if (m_pending == 0)
            done(round);
    });
}

void ModelSearch::search(const QString &terms) {
    cancel();
    m_library.clear();
    m_hub.clear();
    m_problem.clear();
    m_failed.clear();
    m_repoLicence.clear();
    if (!m_settings || !m_capability)
        return;
    // Off is off: nothing is searched either.
    if (m_settings->aiOff()) {
        setState(u"off"_s);
        return;
    }
    const int round = m_round;
    m_place = Discovery::pulled(kind()) ? ModelFit::placeOf(*m_settings, kind()) : std::nullopt;
    setState(u"searching"_s);
    if (!Discovery::pulled(kind())) {
        searchSpeech(terms.simplified(), round);
        return;
    }
    searchLibrary(terms.simplified(), round);
    searchHub(terms.simplified(), round);
}

void ModelSearch::searchLibrary(const QString &terms, int round) {
    QUrl url(m_sites.library + u"/search"_s);
    QUrlQuery query;
    query.addQueryItem(u"q"_s, terms);
    if (const QString category = Discovery::libraryCategory(kind()); !category.isEmpty())
        query.addQueryItem(u"c"_s, category);
    url.setQuery(query);
    get(url.toString(), round, [this, round](int status, const QByteArray &body) {
        if (status != 200)
            return;
        m_library = Discovery::fromLibrary(QString::fromUtf8(body), mostWeighed);
        for (qsizetype at = 0; at < m_library.size(); ++at)
            weigh(at, round);
    });
}

// A model of Ollama's library: its size and licence, from its registry's manifest.
void ModelSearch::weigh(qsizetype at, int round) {
    const Discovery::Found &one = m_library.at(at);
    if (one.hosted || !one.name.contains(u':'))
        return;
    const QString repository = one.repository.contains(u'/') ? one.repository : u"library/"_s + one.repository;
    const QString base = m_sites.registry + u"/v2/"_s + repository;
    get(base + u"/manifests/"_s + one.name.section(u':', 1), round, [this, at, base, round](int status, const QByteArray &body) {
        if (status != 200 || at >= m_library.size())
            return;
        const Discovery::Manifest made = Discovery::fromManifest(QJsonDocument::fromJson(body).object());
        if (made.bytes > 0)
            m_library[at].bytes = made.bytes;
        if (!made.licence.isEmpty())
            get(base + u"/blobs/"_s + made.licence, round, [this, at](int licensed, const QByteArray &text) {
                if (licensed == 200 && at < m_library.size())
                    m_library[at].licence = Discovery::licenceName(QString::fromUtf8(text));
            });
    });
}

void ModelSearch::searchHub(const QString &terms, int round) {
    QUrl url(m_sites.hub + u"/api/models"_s);
    QUrlQuery query;
    if (!terms.isEmpty())
        query.addQueryItem(u"search"_s, terms);
    query.addQueryItem(u"pipeline_tag"_s, Discovery::hubTask(kind()));
    query.addQueryItem(u"filter"_s, u"gguf"_s); // what Ollama can pull
    query.addQueryItem(u"sort"_s, u"downloads"_s);
    query.addQueryItem(u"direction"_s, u"-1"_s);
    query.addQueryItem(u"limit"_s, QString::number(mostWeighed));
    url.setQuery(query);
    get(url.toString(), round, [this, round](int status, const QByteArray &body) {
        if (status != 200)
            return;
        m_hub = Discovery::fromHub(QJsonDocument::fromJson(body).array());
        for (qsizetype at = 0; at < m_hub.size(); ++at) {
            // Without a GGUF Ollama can pull, it is dropped once all have answered.
            m_hub[at].bytes = -1;
            get(m_sites.hub + u"/api/models/"_s + m_hub.at(at).repository + u"/tree/main"_s, round,
                [this, at](int listed, const QByteArray &tree) {
                    const auto gguf = listed == 200 ? Discovery::ggufOf(QJsonDocument::fromJson(tree).array()) : std::nullopt;
                    if (!gguf || at >= m_hub.size())
                        return;
                    if (!gguf->quant.isEmpty())
                        m_hub[at].name += u':' + gguf->quant;
                    m_hub[at].bytes = gguf->bytes;
                });
        }
    });
}

// whisper.cpp's models and Piper's voices: the files of their repositories.
void ModelSearch::searchSpeech(const QString &terms, int round) {
    const QString repository = m_sites.hub + u"/api/models/"_s + Discovery::speechRepository(kind());
    get(repository + u"/tree/main?recursive=true"_s, round, [this, terms](int status, const QByteArray &body) {
        if (status == 200)
            m_hub = Discovery::speechFiles(kind(), QJsonDocument::fromJson(body).array(), terms);
    });
    get(repository, round, [this](int status, const QByteArray &body) {
        if (status != 200)
            return;
        for (const QJsonValue tag : QJsonDocument::fromJson(body).object().value("tags"_L1).toArray())
            if (tag.toString().startsWith("license:"_L1))
                m_repoLicence = tag.toString().mid(8);
    });
}

void ModelSearch::done(int round) {
    if (round != m_round || !m_capability || m_state != "searching"_L1)
        return;
    m_hub.removeIf([](const Discovery::Found &one) { return one.bytes < 0; });
    // What runs on this computer is judged by its memory; a server elsewhere doesn't say its own.
    const bool here = !Discovery::pulled(kind())
                      || (m_place && m_settings
                          && Recognition::reach(m_place->server.baseUrl, m_settings->ownAddresses()) == Recognition::Reach::ThisComputer);
    const Discovery::Free free = here && m_freeHere ? m_freeHere() : Discovery::Free{};
    for (auto *found : {&m_library, &m_hub})
        judge(*found, kind(), free, m_repoLicence);
    if (m_library.isEmpty() && m_hub.isEmpty() && !m_failed.isEmpty()) {
        m_problem = m_failed;
        setState(u"failed"_s);
        return;
    }
    setState(u"found"_s);
}

void ModelSearch::install(int index) {
    const QList<Discovery::Found> all = m_library + m_hub;
    if (!m_settings || !m_capability || m_state == "searching"_L1 || m_state == "installing"_L1 || index < 0 || index >= all.size())
        return;
    const Discovery::Found &found = all.at(index);
    // Ollama's computers are someone else's: only where hosted is allowed for this.
    if (found.hosted && !AiPrivacy::hostedAllowed(*m_settings, kind())) {
        m_problem = Failure(Failure::Code::ModelHosted, {found.name}).text();
        setState(u"failed"_s);
        return;
    }
    if (Discovery::pulled(kind()) && !m_place) {
        m_problem = Failure(Failure::Code::NoServerToInstallOn).text();
        setState(u"failed"_s);
        return;
    }
    m_progress = 0;
    setState(u"installing"_s);
    if (!Discovery::pulled(kind())) {
        fetch(found);
        return;
    }
    const AiServer onto = m_place ? m_place->server : AiServer{};
    m_scout.pull(
        onto, found.name,
        [this](qint64 done, qint64 total) {
            m_progress = total > 0 ? std::clamp(static_cast<double>(done) / static_cast<double>(total), 0.0, 1.0) : 0.0;
            emit changed();
        },
        [this, found](const std::expected<void, Failure> &pulled) {
            if (!pulled) {
                m_problem = pulled.error().text();
                setState(u"failed"_s);
                return;
            }
            choose(found, {});
        });
}

// A speech file, and for a Piper voice its description beside it, downloaded
// where its engine looks, by way of a file of its own until it is whole.
void ModelSearch::fetch(const Discovery::Found &found) {
    const QString folder = speechFolder(kind());
    QDir().mkpath(folder);
    const bool voice = kind() == Capability::TextToSpeech;
    const QString target = folder + u'/' + (voice ? found.name + u".onnx"_s : found.name);
    const QString source = m_sites.hub + u'/' + found.repository + u"/resolve/main/"_s + found.file;
    const auto files = std::make_shared<QList<std::pair<QString, QString>>>(QList<std::pair<QString, QString>>{{source, target}});
    if (voice)
        files->append({source + u".json"_s, target + u".json"_s});
    const int round = m_round;
    const auto next = std::make_shared<std::function<void()>>();
    // Each download holds the next step while it runs; the step only looks itself up.
    *next = [this, files, self = std::weak_ptr(next), found, target, round] {
        const auto next = self.lock();
        if (round != m_round || !next)
            return;
        if (files->isEmpty()) {
            choose(found, target);
            return;
        }
        const auto [from, to] = files->takeFirst();
        const auto part = std::make_shared<QFile>(to + u".part"_s);
        if (!part->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            m_problem = Failure(Failure::Code::ModelNotDownloaded, {found.name, part->errorString()}).text();
            setState(u"failed"_s);
            return;
        }
        QNetworkRequest request{QUrl(from)};
        request.setHeader(QNetworkRequest::UserAgentHeader, u"Omabook"_s);
        QNetworkReply *reply = m_network.get(request);
        m_replies.append(reply);
        connect(reply, &QNetworkReply::readyRead, this, [reply, part] { part->write(reply->readAll()); });
        // The progress shown is the model's own; a voice's description beside it is small.
        connect(reply, &QNetworkReply::downloadProgress, this, [this, main = to == target](qint64 done, qint64 total) {
            if (main && total > 0) {
                m_progress = std::clamp(static_cast<double>(done) / static_cast<double>(total), 0.0, 1.0);
                emit changed();
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, part, to, found, next, round] {
            reply->deleteLater();
            m_replies.removeAll(reply);
            part->write(reply->readAll());
            part->close();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (round != m_round || status != 200) {
                part->remove();
                if (round == m_round) {
                    m_problem = Failure(Failure::Code::ModelNotDownloaded,
                                        {found.name, status == 0 ? reply->errorString() : QString::number(status)})
                                    .text();
                    setState(u"failed"_s);
                }
                return;
            }
            QFile::remove(to);
            part->rename(to);
            (*next)();
        });
    };
    (*next)();
}

void ModelSearch::choose(const Discovery::Found &found, const QString &file) {
    switch (kind()) {
    case Capability::SpeechToText: {
        // whisper.cpp, with the model downloaded.
        QJsonObject recognition = m_settings->recognition();
        recognition.insert(u"whisper_model"_s, file);
        m_settings->setRecognition(recognition);
        if (!m_settings->hasCapabilities())
            AiPrivacy::carryOver(*m_settings);
        QJsonObject all = m_settings->capabilities();
        QJsonObject one = all.value(Capabilities::name(Capability::SpeechToText)).toObject();
        one.insert(u"provider"_s, u"whisper.cpp"_s);
        all.insert(Capabilities::name(Capability::SpeechToText), one);
        m_settings->setCapabilities(all);
        break;
    }
    case Capability::TextToSpeech: {
        // Piper, with the voice downloaded.
        QJsonObject readAloud = m_settings->readAloud();
        readAloud.insert(u"engine"_s, u"piper"_s);
        m_settings->setReadAloud(readAloud);
        Voices::keep(*m_settings, u"voice"_s, found.name);
        break;
    }
    default:
        ModelFit::choose(*m_settings, kind(), found.name);
        break;
    }
    m_model = found.name;
    setState(u"chosen"_s);
    emit chosen(found.name);
}

void ModelSearch::cancel() {
    ++m_round;
    m_pending = 0;
    m_scout.cancel();
    const auto replies = std::exchange(m_replies, {});
    for (const QPointer<QNetworkReply> &reply : replies)
        if (reply)
            reply->abort();
    if (m_state == "searching"_L1 || m_state == "installing"_L1)
        setState(m_library.isEmpty() && m_hub.isEmpty() ? QString() : u"found"_s);
}
