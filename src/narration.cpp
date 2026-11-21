#include "narration.h"

#include <algorithm>

Narration::Narration(int sections, std::function<QString(int section)> read, std::unique_ptr<Voice> voice, QObject *parent)
    : QObject(parent), m_text(sections, std::move(read)), m_voice(std::move(voice)) {
    connect(m_voice.get(), &Voice::spoken, this, &Narration::spoken);
    connect(m_voice.get(), &Voice::failed, this, &Narration::stopped);
}

Narration::~Narration() { m_voice->stop(); }

const QList<Sentences::Sentence> &Narration::sentencesOf(int section) const {
    auto found = m_sentences.find(section);
    if (found == m_sentences.end())
        found = m_sentences.insert(section, Sentences::of(m_text.text(section)));
    return *found;
}

QString Narration::wordsOf(const Spoken &spoken) const {
    const Sentences::Sentence &sentence = spoken.sentence;
    return m_text.text(spoken.section).sliced(sentence.start, sentence.end - sentence.start);
}

std::optional<Narration::Spoken> Narration::firstFrom(int section, qsizetype offset) const {
    for (int at = std::max(0, section); at < m_text.sections(); ++at) {
        const QList<Sentences::Sentence> &sentences = sentencesOf(at);
        const qsizetype index = Sentences::at(sentences, at == section ? offset : 0);
        if (index >= 0)
            return Spoken{.section = at, .sentence = sentences.at(index)};
    }
    return std::nullopt;
}

std::optional<Narration::Spoken> Narration::after(const Spoken &spoken) const { return firstFrom(spoken.section, spoken.sentence.end); }

std::optional<Narration::Spoken> Narration::before(const Spoken &spoken) const {
    const QList<Sentences::Sentence> &here = sentencesOf(spoken.section);
    if (const qsizetype index = here.indexOf(spoken.sentence); index > 0)
        return Spoken{.section = spoken.section, .sentence = here.at(index - 1)};
    for (int section = spoken.section - 1; section >= 0; --section)
        if (const QList<Sentences::Sentence> &sentences = sentencesOf(section); !sentences.isEmpty())
            return Spoken{.section = section, .sentence = sentences.last()};
    return std::nullopt;
}

QString Narration::words() const { return m_current ? wordsOf(*m_current) : QString(); }

Passage Narration::passage() const {
    if (!m_current)
        return {};
    return Passage::of(m_text.text(m_current->section), m_current->section, m_current->sentence.start, m_current->sentence.end);
}

BookPlace Narration::place() const { return m_current ? m_text.placeOf(m_current->section, m_current->sentence.start) : BookPlace{}; }

bool Narration::from(int section, qsizetype offset) {
    const auto found = firstFrom(section, offset);
    if (!found)
        return false;
    moveTo(*found);
    return true;
}

void Narration::moveTo(const Spoken &spoken) {
    m_current = spoken;
    emit moved();
    if (m_playing)
        sayCurrent();
}

void Narration::play() {
    if (!m_current || m_playing)
        return;
    m_playing = true;
    emit playingChanged();
    sayCurrent();
}

void Narration::pause() {
    if (!m_playing)
        return;
    m_playing = false;
    m_voice->stop();
    emit playingChanged();
}

void Narration::next() {
    if (const auto found = m_current ? after(*m_current) : std::nullopt)
        moveTo(*found);
}

void Narration::back() {
    if (const auto found = m_current ? before(*m_current) : std::nullopt)
        moveTo(*found);
}

void Narration::sayCurrent() {
    if (!m_current)
        return;
    m_voice->say(words(), m_how);
    if (const auto upcoming = after(*m_current))
        m_voice->prepare(wordsOf(*upcoming), m_how);
}

void Narration::spoken() {
    if (!m_playing || !m_current)
        return;
    if (const auto found = after(*m_current)) {
        moveTo(*found);
        return;
    }
    m_playing = false;
    emit playingChanged();
    emit finished();
}

void Narration::stopped(const Failure &why) {
    m_playing = false;
    m_voice->stop();
    emit playingChanged();
    emit failed(why);
}
