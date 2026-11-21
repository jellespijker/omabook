#pragma once

#include <QJsonValue>
#include <QObject>
#include <QQuickTextDocument>
#include <QSizeF>
#include <QStringList>
#include <QVariantList>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

#include "failure.h"
#include "narration.h"
#include "voice.h"

class Reader;

// The open book read aloud (#73), as the reader shows it: from the place on
// the page, the sentence being read marked where the page shows it, and the
// page following (the next chapter, or page, opened as it is reached; within
// a chapter the view keeps it in sight, which is what keeps the place).
// Paused, it stays on its sentence, which going on reads again; stopped, the
// next start is from wherever the reader is then. How fast, and in which
// voice, is kept in the settings (Voices).
class ReadAloud : public QObject {
    Q_OBJECT
    // Whether the book is being read aloud, or paused partway.
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(bool speaking READ speaking NOTIFY stateChanged)
    // The section of the sentence being read, -1 for none; and a count that
    // changes with every sentence, for a view to mark the new one.
    Q_PROPERTY(int section READ section NOTIFY sentenceChanged)
    Q_PROPERTY(int sentence READ sentence NOTIFY sentenceChanged)
    Q_PROPERTY(QString words READ words NOTIFY sentenceChanged)
    // What reads, as the panel names it ("Piper"), or why nothing can.
    Q_PROPERTY(QString engine READ engine NOTIFY choicesChanged)
    Q_PROPERTY(QString why READ why NOTIFY choicesChanged)
    Q_PROPERTY(QStringList voices READ voices NOTIFY choicesChanged)
    Q_PROPERTY(QString voice READ voice WRITE setVoice NOTIFY choicesChanged)
    Q_PROPERTY(double rate READ rate WRITE setRate NOTIFY choicesChanged)

public:
    using VoiceMaker = std::function<std::expected<std::unique_ptr<Voice>, Failure>()>;

    explicit ReadAloud(Reader *reader);
    ~ReadAloud() override;
    ReadAloud(const ReadAloud &) = delete;
    ReadAloud &operator=(const ReadAloud &) = delete;
    ReadAloud(ReadAloud &&) = delete;
    ReadAloud &operator=(ReadAloud &&) = delete;

    // What makes the voice: the settings' choice (Voices::choose), or a
    // stand-in in the tests, which names itself as the engine.
    void setVoiceMaker(VoiceMaker maker, QString name);

    [[nodiscard]] bool active() const noexcept { return m_narration != nullptr; }
    [[nodiscard]] bool speaking() const { return m_narration && m_narration->playing(); }
    [[nodiscard]] int section() const;
    [[nodiscard]] int sentence() const noexcept { return m_sentences; }
    [[nodiscard]] QString words() const { return m_narration ? m_narration->words() : QString(); }
    [[nodiscard]] QString engine() const { return m_engine; }
    [[nodiscard]] QString why() const { return m_why; }
    [[nodiscard]] QStringList voices() const { return m_voices; }
    [[nodiscard]] QString voice() const;
    void setVoice(const QString &voice);
    [[nodiscard]] double rate() const;
    void setRate(double rate);
    // The sentence being read, by its section and characters there; none while nothing is.
    [[nodiscard]] std::optional<Narration::Spoken> current() const;

    // Reads aloud from the reader's place, pauses, or goes on: what its key does.
    // `atTop` is the words at the top of the page, where a flowing book is read from.
    Q_INVOKABLE void toggle(const QString &atTop = {});
    Q_INVOKABLE void start(const QString &atTop = {});
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    Q_INVOKABLE void back();
    // Looks again at what can read aloud: what is installed, and the settings.
    Q_INVOKABLE void refresh();
    // Where the sentence being read stands on the chapter a TextEdit shows,
    // and on a page of a PDF as it is drawn: a rectangle a line; none
    // when it isn't there.
    Q_INVOKABLE QVariantList rectsIn(QQuickTextDocument *chapter) const;
    Q_INVOKABLE QVariantList rectsOnPage(int page, QSizeF shown) const;

signals:
    void stateChanged();
    void sentenceChanged();
    void choicesChanged();

private:
    void moved();
    void failed(const Failure &why);
    // Where in a section the words at the top of the page start; -1 when they aren't found.
    [[nodiscard]] qsizetype offsetOf(int section, const QString &atTop) const;
    void choose(const QString &key, const QJsonValue &value);

    Reader *m_reader;
    VoiceMaker m_maker;
    QString m_makerName;
    std::unique_ptr<Narration> m_narration;
    qint64 m_bookId = 0;
    int m_sentences = 0;
    // The reader is being moved to the sentence's section, not by the person.
    bool m_following = false;
    QString m_engine;
    QString m_why;
    QStringList m_voices;
};
