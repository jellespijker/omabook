#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "ai.h"
#include "markupfilter.h"

// One question whose whole answer is wanted at once: it owns its provider,
// gathers the pieces, and says when the answer is complete. An answer for a
// person to read (AiRequest::prose) has the markup some models wrap it in
// taken out on the way, in the pieces as in the whole.
class AiCall : public QObject {
    Q_OBJECT

public:
    explicit AiCall(std::unique_ptr<AiProvider> provider, QObject *parent = nullptr);

    void ask(const AiRequest &request);
    void cancel();

signals:
    void delta(const QString &text);
    void done(const QString &answer);
    void failed(const Failure &failure);
    void noted(const Failure &note);

private:
    void gather(const QString &text);

    std::unique_ptr<AiProvider> m_provider;
    QString m_answer;
    bool m_prose = false;
    MarkupFilter m_markup;
};
