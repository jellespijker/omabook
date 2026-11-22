#pragma once

#include <QJSEngine>
#include <QJSValue>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <memory>

#include "aicapability.h"
#include "recognition.h"

class AiProvider;
class RecognitionQueue;
class ScriptPlugin;
class Transcriber;
class Voice;

// The C++ side of `omabook.ai` and `omabook.recognize` (#84), beside
// ScriptHost (scriptplugin.cpp), which hands the script what scriptassist.js
// builds from it: all six capabilities (ADR 0008) through the registry, with
// the provider the person chose for each: asking the model, having ink and
// recordings recognised, reading aloud and embeddings. A plugin never picks a
// provider, and sends to a hosted one only as AiPrivacy has it: for answers
// and quick the person's choice stands unless it says `hosted: false`; for
// the other four, where it sends the person's own material whole, it must
// also ask for one with `hosted: true`. A plugin's recognitions are its own: a recording is
// a file of its own folder, and what is recognised is kept with its data
// (RecognitionStore), which go between one's computers as the rest of it does.
// Recognition goes through a queue of the plugin's own (RecognitionQueue),
// made when it is first asked for.
class ScriptAi : public QObject {
    Q_OBJECT

public:
    explicit ScriptAi(ScriptPlugin *plugin);
    ~ScriptAi() override;
    ScriptAi(const ScriptAi &) = delete;
    ScriptAi &operator=(const ScriptAi &) = delete;
    ScriptAi(ScriptAi &&) = delete;
    ScriptAi &operator=(ScriptAi &&) = delete;

    // `{ai, recognize}` for the `omabook` object, made in the plugin's engine.
    [[nodiscard]] QJSValue build(QJSEngine &engine);

    Q_INVOKABLE bool available();
    // Whether a capability may be used (#138, ADR 0008): the registry's
    // status, {capability, available, hosted, provider, model, why, message},
    // `why` a code (Capabilities::lackName, or "unknown-capability"); `ai.status`.
    // `options` is JSON: {capability, model, hosted}; the capability is answers
    // (quick for the model "quick") unless named.
    Q_INVOKABLE QVariantMap status(const QString &options);
    // `options` is JSON: {system, model, hosted}, `hosted` as for status().
    Q_INVOKABLE void ask(const QString &prompt, const QString &options, const QJSValue &onText, const QJSValue &resolve,
                         const QJSValue &reject);

    // Whether there is a way to recognise "ink" or "audio" that may be used,
    // for one who asked for a hosted server or not: `recognize.available`.
    Q_INVOKABLE bool recognizes(const QString &kind, bool hosted);
    // The same, saying why not (#133): {available, why, message}, `why` a
    // code (RecognitionRoute::lackName, or "unknown-kind", "no-settings"),
    // `message` its plain words; `recognize.status`.
    Q_INVOKABLE QVariantMap recognition(const QString &kind, bool hosted);
    // Each JSON here is what scriptassist.js made with JSON.stringify; the
    // options are {language, hosted, again}. Answers the request's number.
    Q_INVOKABLE int ink(const QString &strokes, const QString &options, const QJSValue &onWaiting, const QJSValue &resolve,
                        const QJSValue &reject);
    Q_INVOKABLE int audio(const QString &file, const QString &options, const QJSValue &onWaiting, const QJSValue &resolve,
                          const QJSValue &reject);
    Q_INVOKABLE bool cancel(int job);
    // A recording of the plugin's written out whole (#151), `recognize.transcript`:
    // a piece at a time in the background, each kept as it is written out, so
    // one stopped goes on from there when asked again; kept whole, it is
    // answered at once. `options` is JSON: {language, hosted, again}; told
    // how far it is with onProgress(done, total). Answers the request's number.
    Q_INVOKABLE int transcript(const QString &file, const QString &options, const QJSValue &onProgress, const QJSValue &onWaiting,
                               const QJSValue &resolve, const QJSValue &reject);
    // Stops it, keeping what was written out; its promise is rejected.
    Q_INVOKABLE bool stopTranscript(int job);

    // Reads `text` aloud with the voice the person chose (`ai.speak`); a new
    // one stops the one before. `options` is JSON: {language, hosted}. The
    // window shows it, naming the plugin, and can stop it (AudioDevices, #147).
    Q_INVOKABLE void speak(const QString &text, const QString &options, const QJSValue &resolve, const QJSValue &reject);
    Q_INVOKABLE void stopSpeaking();
    // The embeddings of each text (`ai.embed`), on a thread of their own;
    // `texts` and `options` ({use, hosted}) are JSON.
    Q_INVOKABLE void embed(const QString &texts, const QString &options, const QJSValue &resolve, const QJSValue &reject);

private:
    // The status of the capability `options` names, as the plugin asks it.
    // `provider`, when given, is handed the model to ask when it may be.
    [[nodiscard]] QVariantMap statusOf(Capability capability, const QJsonObject &options,
                                       std::unique_ptr<AiProvider> *provider = nullptr);
    // The reading aloud ended: spoken (no why), stopped, or failed with why.
    void settleSpeech(const QString &why);

    int submit(Recognition::Input input, const QJsonObject &options, const QJSValue &onWaiting, const QJSValue &resolve,
               const QJSValue &reject);
    [[nodiscard]] RecognitionQueue *queue();
    // The window's, which writes out one recording at a time for everyone; else one of its own.
    [[nodiscard]] Transcriber *transcriber();
    void told(const QJSValue &callback, const QJSValueList &arguments);

    ScriptPlugin *m_plugin;
    std::unique_ptr<RecognitionQueue> m_queue;
    std::unique_ptr<Transcriber> m_transcriber;
    QList<int> m_transcripts; // its own, under way or waiting their turn
    // What is being read aloud, and what is told when it ends.
    std::unique_ptr<Voice> m_voice;
    QJSValue m_spoken, m_notSpoken;
};
