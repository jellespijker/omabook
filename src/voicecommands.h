#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <memory>
#include <optional>

#include "voicegrammar.h"

class AiCall;
class AiProvider;

// A command said while reading (#150), understood: by the grammar of the
// five languages first (VoiceGrammar), and only what it doesn't know by the
// model, as quick work through the registry (`Answerer`, AiRegistry's), so
// it goes to a hosted model only where the person allowed hosted for quick
// work, and to none with the AI Off. The model is sent the words said and
// the commands it may answer with, nothing of the book. What neither
// understands is said not understood.
class VoiceCommands : public QObject {
    Q_OBJECT
    // The model is working out what was said.
    Q_PROPERTY(bool asking READ asking NOTIFY askingChanged)

public:
    using Answerer = std::function<std::unique_ptr<AiProvider>(QString *why)>;

    explicit VoiceCommands(Answerer answerer, QObject *parent = nullptr);
    ~VoiceCommands() override;
    VoiceCommands(const VoiceCommands &) = delete;
    VoiceCommands &operator=(const VoiceCommands &) = delete;
    VoiceCommands(VoiceCommands &&) = delete;
    VoiceCommands &operator=(VoiceCommands &&) = delete;

    // The model asked when the grammar doesn't know what was said (a test's).
    void setAnswerer(Answerer answerer) { m_answerer = std::move(answerer); }

    // What `said` is a command for, in `language` (the window's) first:
    // understood() at once when the grammar knows it, else once the model
    // has answered; notUnderstood() when neither knows it. One asked before
    // and not answered yet is let go of.
    Q_INVOKABLE void understand(const QString &said, const QString &language);
    // Stops asking the model; nothing is told.
    Q_INVOKABLE void cancel();
    // The entry of `contents` (Reader::toc) that is chapter `number`; -1 for none.
    Q_INVOKABLE int chapter(const QVariantList &contents, int number) const;

    [[nodiscard]] bool asking() const noexcept { return m_call != nullptr; }

    // What the model is told: the commands it may answer with, each with what it does.
    [[nodiscard]] static QString instructions();
    // The command in the model's answer ("reader.next", "spoken.chapter 5",
    // "spoken.meaning serendipity"), or nothing for "none" or anything else.
    [[nodiscard]] static std::optional<VoiceGrammar::Heard> parse(const QString &answer);

signals:
    // `command` is one of the table's or VoiceGrammar's spoken ones;
    // `number` a chapter's, `words` the word asked about; `said` as heard.
    void understood(const QString &command, int number, const QString &words, const QString &said);
    void notUnderstood(const QString &said);
    void askingChanged();

private:
    void ask(const QString &said);
    void answered(const QString &said, const QString &answer);
    void ended();

    Answerer m_answerer;
    std::unique_ptr<AiCall> m_call;
    QString m_said; // what the model is asked about
    QTimer m_patience;
};
