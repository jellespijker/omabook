#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QtTypes>
#include <expected>
#include <functional>

#include "aiendpoint.h"
#include "failure.h"
#include "modelfit.h"

class QNetworkReply;

// What a server has, and fetching a model onto it (#172): Ollama's own API
// (/api/tags and /api/show for its models and what each does, /api/pull to
// download one, with its progress), else the OpenAI-compatible /models, which
// gives only names. Nothing of the person's is sent: only model names. The
// answers come back on the thread that asked, to the callbacks given; none
// is called once cancel() was.
class ModelScout : public QObject {
    Q_OBJECT

public:
    explicit ModelScout(QObject *parent = nullptr);

    struct Listing {
        bool ollama = false; // Ollama's own API answered: a model can be pulled onto it
        QList<ModelFit::Model> models;
    };
    using Listed = std::function<void(const std::expected<Listing, Failure> &)>;
    void list(const AiServer &server, Listed done);

    // Bytes done of the total, as Ollama says them for the layer it is fetching.
    using Progress = std::function<void(qint64 done, qint64 total)>;
    using Pulled = std::function<void(const std::expected<void, Failure> &)>;
    void pull(const AiServer &server, const QString &model, Progress progress, Pulled done);

    // Stops what is under way; its callbacks are not called.
    void cancel();

private:
    void show(const AiServer &server, Listing listing, const Listed &done);
    void openAiModels(const AiServer &server, Listed done);
    [[nodiscard]] QNetworkReply *sent(const AiServer &server, const QString &url, const QByteArray &body = {});

    QNetworkAccessManager m_network;
    QList<QPointer<QNetworkReply>> m_replies;
    int m_round = 0; // a cancel() starts a new one; answers of an old one are dropped
};
