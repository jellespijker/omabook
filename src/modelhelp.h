#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <optional>

#include "aicapability.h"
#include "modelfit.h"
#include "modelscout.h"

class Settings;

// A capability whose model isn't on its server, mended where the failure
// shows (#172): Find's pane, the AI page's row, a plugin (`ai.status`'s
// `remedy`, `ai.remedy`), and #170's Test. It asks the server what it has;
// where the model asked for is missing it offers one already there that fits
// the capability (by what Ollama says each does, else by its family), else a
// small, well-known one to download onto an Ollama server, its size said
// first, with its progress; once there, it is chosen for the capability.
// Only model names go to the server; with the AI Off nothing is asked.
class ModelHelp : public QObject {
    Q_OBJECT
    // The window's settings, which a choice is written to.
    Q_PROPERTY(QObject *settings READ settings WRITE setSettings NOTIFY settingsChanged)
    // By its name: "embeddings", "handwriting", ...
    Q_PROPERTY(QString capability READ capability WRITE setCapability NOTIFY settingsChanged)
    // "" (not looked), "looking", "none" (not a server's model: nothing to look at),
    // "there" (the model asked for is on the server), "offer" (another on it fits:
    // `model`, among `fitting`), "suggest" (`model` to download, of `size`),
    // "missing" (nothing on it fits, and it can't be sent one), "downloading"
    // (`progress`), "chosen" (`model` was chosen for it), or "failed" (`problem`).
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString wanted READ wanted NOTIFY changed)
    // Whether `wanted` is on the server, and can't do this (a model that doesn't read pictures).
    Q_PROPERTY(bool unfit READ unfit NOTIFY changed)
    Q_PROPERTY(QString model READ model NOTIFY changed)
    Q_PROPERTY(QStringList fitting READ fitting NOTIFY changed)
    Q_PROPERTY(QString size READ size NOTIFY changed)
    Q_PROPERTY(QString server READ server NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)

public:
    explicit ModelHelp(QObject *parent = nullptr);
    ModelHelp(Settings *settings, Capability capability, QObject *parent = nullptr);

    [[nodiscard]] QObject *settings() const;
    void setSettings(QObject *settings);
    [[nodiscard]] QString capability() const;
    void setCapability(const QString &name);
    [[nodiscard]] QString state() const { return m_state; }
    [[nodiscard]] QString wanted() const { return m_wanted; }
    [[nodiscard]] bool unfit() const { return m_unfit; }
    [[nodiscard]] QString model() const { return m_model; }
    [[nodiscard]] QStringList fitting() const { return m_fitting; }
    [[nodiscard]] QString size() const;
    [[nodiscard]] QString server() const { return m_place ? m_place->server.host() : QString(); }
    [[nodiscard]] double progress() const { return m_progress; }
    [[nodiscard]] QString problem() const { return m_problem; }

    // Asks the server again what it has.
    Q_INVOKABLE void look();
    // Chooses a model on the server for the capability: the one offered, unless named.
    Q_INVOKABLE void use(const QString &model = {});
    // Downloads the model suggested onto the Ollama server, then chooses it.
    Q_INVOKABLE void download();
    // Stops a look or a download; a download stopped is offered again.
    Q_INVOKABLE void cancel();

    // What can be done, as a plugin is told it: {action ("use" or "download"),
    // model, size (in bytes), server}; empty while there is nothing to do.
    [[nodiscard]] QVariantMap remedy() const;

signals:
    void settingsChanged();
    void changed();
    // The model was chosen for the capability, there or downloaded.
    void chosen(const QString &model);

private:
    void listed(Capability capability, const std::expected<ModelScout::Listing, Failure> &listing);
    void setState(const QString &state);

    QPointer<Settings> m_settings;
    std::optional<Capability> m_capability;
    std::optional<ModelFit::Place> m_place;
    ModelScout m_scout;
    QString m_state, m_wanted, m_model, m_problem;
    QStringList m_fitting;
    qint64 m_bytes = 0;
    double m_progress = 0;
    bool m_unfit = false;
};
