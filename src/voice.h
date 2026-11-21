#pragma once

#include <QObject>
#include <QString>

#include "failure.h"

// How a sentence is read aloud: how fast (1 is the voice's own pace, from
// half that to twice it), in which of the engine's voices (empty for the one
// it would take, `language`'s first when it has one) and the book's
// language, as its details say it ("en", "nl-NL"; empty when unknown).
struct Speech {
    double rate = 1.0;
    QString voice;
    QString language;

    bool operator==(const Speech &) const = default;
};

// What reads a book aloud, a sentence at a time (#73): Piper or
// speech-dispatcher on this computer, a server the person chose, or a
// stand-in in the tests. Each sentence it is given ends in spoken() or in
// failed(), once, unless it is stopped first.
class Voice : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Reads a sentence; one being read is stopped first.
    virtual void say(const QString &sentence, const Speech &how) = 0;
    // The sentence that will most likely be said next, for an engine that
    // takes a moment to start one to make ready while this one is heard.
    virtual void prepare(const QString & /*sentence*/, const Speech & /*how*/) {}
    // Stops at once; nothing is said of the sentence it was reading.
    virtual void stop() = 0;
    // What it is, as the panel shows it: "Piper", "speech-dispatcher", a server's host.
    [[nodiscard]] virtual QString name() const = 0;

signals:
    void spoken();
    void failed(const Failure &why);
};
