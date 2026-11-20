#include "aicall.h"

AiCall::AiCall(std::unique_ptr<AiProvider> provider, QObject *parent) : QObject(parent), m_provider(std::move(provider)) {
    connect(m_provider.get(), &AiProvider::delta, this, [this](const QString &text) { gather(m_prose ? m_markup.take(text) : text); });
    connect(m_provider.get(), &AiProvider::finished, this, [this] {
        if (m_prose)
            gather(m_markup.finish());
        emit done(m_answer);
    });
    connect(m_provider.get(), &AiProvider::failed, this, &AiCall::failed);
    connect(m_provider.get(), &AiProvider::noted, this, &AiCall::noted);
}

void AiCall::ask(const AiRequest &request) {
    m_answer.clear();
    m_prose = request.prose;
    m_markup = {};
    m_provider->ask(request);
}

void AiCall::cancel() { m_provider->cancel(); }

void AiCall::gather(const QString &text) {
    if (text.isEmpty())
        return;
    m_answer += text;
    emit delta(text);
}
