#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <memory>

#include "ai.h"
#include "aicall.h"
#include "blurb.h"
#include "graphbuilder.h"
#include "markupfilter.h"

class Reader;

// The reading companion: a conversation about the open book, grounded in
// what the reader has read so far and nothing beyond it.
class Assistant : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(QString providerName READ providerName NOTIFY availabilityChanged)
    // Why there is no AI, when there is none.
    Q_PROPERTY(QString problem READ problem NOTIFY availabilityChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    // The answer being written; it joins `messages` when it is complete.
    Q_PROPERTY(QString streaming READ streaming NOTIFY streamingChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString note READ note NOTIFY messagesChanged)
    // Whether the knowledge graph of the open book is being brought up to date.
    Q_PROPERTY(bool graphBusy READ graphBusy NOTIFY graphChanged)
    // Whether the AI is thinking of questions to suggest about a selection.
    Q_PROPERTY(bool suggesting READ suggesting NOTIFY suggestingChanged)

public:
    using Factory = std::function<std::unique_ptr<AiProvider>(QString *why)>;

    Assistant(Reader *reader, Factory factory, QObject *parent = nullptr);

    bool available() const { return m_provider != nullptr; }
    QString providerName() const { return m_provider ? m_provider->name() : QString(); }
    QString problem() const noexcept { return m_problem; }
    QVariantList messages() const;
    QString streaming() const noexcept { return m_streaming; }
    bool busy() const noexcept { return m_busy; }
    QString note() const noexcept { return m_note; }
    bool graphBusy() const { return m_graph.busy(); }
    bool suggesting() const noexcept { return m_suggesting != nullptr; }
    // How the knowledge graph grows: "auto" (as chapters are finished and
    // with every catch-up), "ask" (only when asked to) or "off" (not at
    // all: nothing is read from it or written to it either). It is asked
    // for each time, so a change of the setting is followed at once; off
    // until main.cpp says otherwise, so nothing spends AI calls unasked.
    void setGraphMode(std::function<QString()> mode) { m_graphMode = std::move(mode); }
    QString graphMode() const { return m_graphMode ? m_graphMode() : QString("off"); }
    // How long after a chapter is finished the graph takes it in; tests don't wait.
    void setGraphWait(int milliseconds) { m_graphSoon.setInterval(milliseconds); }

    // Asks about the chapter in front of the reader ("chapter") or everything
    // read so far ("sofar"), with the passage they selected, if any.
    Q_INVOKABLE void ask(const QString &question, const QString &scope = "chapter", const QString &selection = {});
    // Brings the reader back up to speed. The first time it reads the book
    // so far; after that it is sent its own notes from last time and only
    // what was read since, and with nothing new read it costs nothing.
    Q_INVOKABLE void catchUp();
    // Who or what selected words are, for a name the knowledge graph doesn't
    // know: one short question with only the words and their paragraph.
    Q_INVOKABLE void identify(const QString &selection, const QString &paragraph);
    // Three more questions worth asking about a selection, from the quick
    // model, kept with the reader's marks under `key` (see
    // ReadingMarks::suggestionKey) so the same thing at the same place is
    // never asked about twice.
    Q_INVOKABLE void suggestMore(const QString &key, qint64 thing, const QString &selection, const QString &paragraph);
    // Who is who, as far as the reader has got: kept and brought up to
    // date the same way, so it never knows more than they have read.
    Q_INVOKABLE void whoIsWho();
    // Brings the knowledge graph of the open book up to where the reader
    // is, sending only what was read since it was last built.
    Q_INVOKABLE void growGraph();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clear();
    // Picks the provider up again, after the settings changed.
    Q_INVOKABLE void refresh();

signals:
    void availabilityChanged();
    void messagesChanged();
    void streamingChanged();
    void busyChanged();
    // The graph is being built, or took in what a request found.
    void graphChanged();
    void suggestingChanged();

private:
    void setBusy(bool busy);
    void finishAnswer();

    Reader *m_reader;
    Factory m_factory;
    std::unique_ptr<AiProvider> m_provider;
    QString m_problem;
    // What is shown: the reader's questions as asked, and the answers.
    QList<AiMessage> m_messages;
    QString m_streaming;
    MarkupFilter m_markup; // takes markup out of the answer being written
    QString m_note;
    bool m_busy = false;
    void askFromNotes(const QString &kind, const QString &asked);
    // `first` says no notes of this kind were kept before, so what is known
    // of the earlier books of the series goes along.
    void sendFromNotes(const QString &kind, const QString &notes, const QString &since, bool first);
    void grow(bool asked);
    // The look-up of earlier books of the series that is under way, if any.
    std::unique_ptr<BlurbSearch> m_search;
    // The kind of notes the answer being written will be kept as, if any.
    QString m_noting;
    bool m_failed = false;
    // The knowledge graph: what builds it beside the reader, and the wait
    // after a chapter is finished before it does.
    std::function<QString()> m_graphMode;
    GraphBuilder m_graph;
    QTimer m_graphSoon;
    int m_lastSection = -1;
    // The question being answered and where it was asked, to be written
    // into the graph once the answer is whole.
    QString m_asked;
    GraphPlace m_askedAt;
    // The request for suggestions under way, if any.
    std::unique_ptr<AiCall> m_suggesting;
};
