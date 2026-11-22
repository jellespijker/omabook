#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <functional>
#include <optional>

#include "aicapability.h"
#include "modeldiscovery.h"
#include "modelfit.h"
#include "modelscout.h"

class QNetworkReply;
class Settings;

// "Find a model…" for one capability (#175, ADR 0008): searches Ollama's
// library and Hugging Face for models that can do it, each with its size, the
// memory it needs, whether that fits the computer it would run on, its
// licence, and whether it runs on someone else's computer; then installs one
// with a press, onto the capability's Ollama server (`/api/pull`, its progress
// shown) or, for speech, where whisper.cpp and Piper keep theirs, and chooses
// it for the capability. The window then tests it once (#170).
//
// Nothing is asked of the network until the person searches, and then only
// the words they searched for and what kind of model the capability takes go
// out, never a book or a note; with the AI Off nothing is searched. A model
// that runs on Ollama's computers (":cloud") is installed only where hosted is
// allowed for the capability.
class ModelSearch : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *settings READ settings WRITE setSettings NOTIFY settingsChanged)
    // By its name: "embeddings", "handwriting", ...
    Q_PROPERTY(QString capability READ capability WRITE setCapability NOTIFY settingsChanged)
    // "" (not searched), "off" (the AI is Off), "searching", "found", "failed"
    // (`problem`), "installing" (`progress`), "chosen" (`model`).
    Q_PROPERTY(QString state READ state NOTIFY changed)
    // Each a Discovery::Found's map (modeldiscovery.h), in the order to show.
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)
    // Where it would run: the server's host, or "" for this computer.
    Q_PROPERTY(QString where READ where NOTIFY changed)
    // Whether a model found can be installed there: an Ollama server to pull
    // onto, or this computer's speech.
    Q_PROPERTY(bool installable READ installable NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(QString model READ model NOTIFY changed)

public:
    explicit ModelSearch(QObject *parent = nullptr);
    ModelSearch(Settings *settings, Capability capability, QObject *parent = nullptr);
    ~ModelSearch() override;
    ModelSearch(const ModelSearch &) = delete;
    ModelSearch &operator=(const ModelSearch &) = delete;
    ModelSearch(ModelSearch &&) = delete;
    ModelSearch &operator=(ModelSearch &&) = delete;

    [[nodiscard]] QObject *settings() const;
    void setSettings(QObject *settings);
    [[nodiscard]] QString capability() const;
    void setCapability(const QString &name);
    [[nodiscard]] QString state() const { return m_state; }
    [[nodiscard]] QVariantList results() const;
    [[nodiscard]] QString where() const;
    [[nodiscard]] bool installable() const;
    [[nodiscard]] double progress() const { return m_progress; }
    [[nodiscard]] QString problem() const { return m_problem; }
    [[nodiscard]] QString model() const { return m_model; }

    // Where it searches, for a test: Ollama's site, its registry, and the Hub.
    struct Sites {
        QString library = QStringLiteral("https://ollama.com");
        QString registry = QStringLiteral("https://registry.ollama.ai");
        QString hub = QStringLiteral("https://huggingface.co");
    };
    void useSites(const Sites &sites) { m_sites = sites; }
    // The memory free where it would run, for a test in place of this computer's.
    void useFree(const std::function<Discovery::Free()> &free) { m_freeHere = free; }

    // Searches for models that can do the capability, by these words.
    Q_INVOKABLE void search(const QString &terms);
    // Installs the result at `index`, then chooses it for the capability.
    Q_INVOKABLE void install(int index);
    // Stops a search or an install; what was found stays.
    Q_INVOKABLE void cancel();

signals:
    void settingsChanged();
    void changed();
    // The model was installed and chosen for the capability.
    void chosen(const QString &model);

private:
    void searchLibrary(const QString &terms, int round);
    void weigh(qsizetype at, int round);
    void searchHub(const QString &terms, int round);
    void searchSpeech(const QString &terms, int round);
    void done(int round);
    void fetch(const Discovery::Found &found);
    void choose(const Discovery::Found &found, const QString &file);
    void setState(const QString &state);
    // The capability searched for; only asked once it is known.
    [[nodiscard]] Capability kind() const;
    // Sends a GET and calls `then` with its body once it answers, if the round still stands.
    void get(const QString &url, int round, const std::function<void(int status, const QByteArray &body)> &then);

    QPointer<Settings> m_settings;
    std::optional<Capability> m_capability;
    std::optional<ModelFit::Place> m_place;
    Sites m_sites;
    std::function<Discovery::Free()> m_freeHere;
    QNetworkAccessManager m_network;
    ModelScout m_scout;
    QList<QPointer<QNetworkReply>> m_replies;
    QList<Discovery::Found> m_library, m_hub;
    int m_round = 0;
    int m_pending = 0;
    QString m_state, m_problem, m_model, m_failed;
    QString m_repoLicence; // for speech, its repository's licence, which each file has
    double m_progress = 0;
};
