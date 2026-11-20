#pragma once

#include <QJSEngine>
#include <QJSValue>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <memory>

#include "recognition.h"

class RecognitionQueue;
class ScriptPlugin;

// The C++ side of `omabook.ai` and `omabook.recognize` (#84), beside
// ScriptHost (scriptplugin.cpp), which hands the script what scriptassist.js
// builds from it: asking the model the person chose, and having ink and
// recordings recognised. A plugin's recognitions are its own: a recording is
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
    Q_INVOKABLE void ask(const QString &prompt, const QString &system, const QString &model, const QJSValue &onText,
                         const QJSValue &resolve, const QJSValue &reject);

    // Whether there is a way to recognise "ink" or "audio" that may be used.
    Q_INVOKABLE bool recognizes(const QString &kind);
    // Each JSON here is what scriptassist.js made with JSON.stringify; the
    // options are {language, hosted, again}. Answers the request's number.
    Q_INVOKABLE int ink(const QString &strokes, const QString &options, const QJSValue &onWaiting, const QJSValue &resolve,
                        const QJSValue &reject);
    Q_INVOKABLE int audio(const QString &file, const QString &options, const QJSValue &onWaiting, const QJSValue &resolve,
                          const QJSValue &reject);
    Q_INVOKABLE bool cancel(int job);

private:
    int submit(Recognition::Input input, const QJsonObject &options, const QJSValue &onWaiting, const QJSValue &resolve,
               const QJSValue &reject);
    [[nodiscard]] RecognitionQueue *queue();

    ScriptPlugin *p;
    std::unique_ptr<RecognitionQueue> m_queue;
};
