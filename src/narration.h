#pragma once

#include <QHash>
#include <QObject>
#include <functional>
#include <memory>
#include <optional>

#include "booktext.h"
#include "failure.h"
#include "sentences.h"
#include "voice.h"

// A book read aloud, a sentence at a time, by a voice (#73): from a place
// on, sentence after sentence and chapter after chapter (or page after
// page), until it is paused, stopped or at the book's end. It says which
// sentence is being read as it goes, by its section and its characters in
// that section's text (Document::text), so the reader can mark it and keep
// the place; pausing and going on start the sentence again, and it can step
// a sentence back or on. The text is read as the book has it, without the
// soft hyphens a page is set with, so hyphenation changes nothing of it.
class Narration : public QObject {
    Q_OBJECT

public:
    // A sentence of a section.
    struct Spoken {
        int section = 0;
        Sentences::Sentence sentence;
    };

    // A book of `sections` sections, each read by `read` when it is needed,
    // the voice that reads it, and what a line break in its text is (a
    // PDF's pages wrap their lines).
    Narration(int sections, std::function<QString(int section)> read, std::unique_ptr<Voice> voice,
              Sentences::Lines lines = Sentences::Lines::End, QObject *parent = nullptr);
    ~Narration() override;
    Narration(const Narration &) = delete;
    Narration &operator=(const Narration &) = delete;
    Narration(Narration &&) = delete;
    Narration &operator=(Narration &&) = delete;

    // To the sentence a character of a section is in (or the first after
    // it, on into the sections after); false when nothing is left to read
    // from there. Read on from there if it was being read.
    bool from(int section, qsizetype offset);
    void play();
    void pause();
    // A sentence on, or back (to the start of the book at most).
    void next();
    void back();
    void setSpeech(const Speech &how) { m_how = how; }

    [[nodiscard]] bool playing() const noexcept { return m_playing; }
    [[nodiscard]] std::optional<Spoken> current() const noexcept { return m_current; }
    // The words of the sentence being read, and it as a passage of its section, to find on a page.
    [[nodiscard]] QString words() const;
    [[nodiscard]] Passage passage() const;
    [[nodiscard]] BookPlace place() const;
    [[nodiscard]] QString voiceName() const { return m_voice->name(); }

signals:
    // Another sentence is being read, or will be when it goes on.
    void moved();
    void playingChanged();
    // It read the book's last sentence.
    void finished();
    void failed(const Failure &why);

private:
    [[nodiscard]] const QList<Sentences::Sentence> &sentencesOf(int section) const;
    [[nodiscard]] QString wordsOf(const Spoken &spoken) const;
    [[nodiscard]] std::optional<Spoken> firstFrom(int section, qsizetype offset) const;
    [[nodiscard]] std::optional<Spoken> after(const Spoken &spoken) const;
    [[nodiscard]] std::optional<Spoken> before(const Spoken &spoken) const;
    void moveTo(const Spoken &spoken);
    void sayCurrent();
    void spoken();
    void stopped(const Failure &why);

    BookText m_text;
    std::unique_ptr<Voice> m_voice;
    Sentences::Lines m_lines = Sentences::Lines::End;
    Speech m_how;
    std::optional<Spoken> m_current;
    bool m_playing = false;
    mutable QHash<int, QList<Sentences::Sentence>> m_sentences;
};
