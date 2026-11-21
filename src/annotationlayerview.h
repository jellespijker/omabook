#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QString>
#include <QVariantList>

#include "annotationlayers.h"

class Reader;
class Settings;

// Others' annotations on the open book (#85), for the reading view: each
// person's highlights and margin notes as far as the reader has read
// (AnnotationLayers), in that person's colour and under their name, beside
// the reader's own and never among them. They are switched off all at once,
// or one person at a time, in the settings (Settings::othersAnnotations,
// hiddenAnnotators), which every computer of the reader's keeps for itself.
//
// Their words are shown as plain text (AnnotationLayers::plain), never as
// markup. A margin note of theirs can be kept as a fact of the graph
// (AnnotationFacts), which then is the reader's own.
class AnnotationLayerView : public QObject {
    Q_OBJECT
    // The window's reader and settings.
    Q_PROPERTY(QObject *reader READ reader WRITE setReader NOTIFY changed)
    Q_PROPERTY(QObject *settings READ settings WRITE setSettings NOTIFY changed)
    // Everyone with annotations on the open book: [{person, called, color, count, shown}].
    Q_PROPERTY(QVariantList people READ people NOTIFY changed)
    // Whether others' annotations are shown at all.
    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY changed)
    // How many of theirs are shown on the open book, and of those how many are margin notes.
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int notes READ notes NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    explicit AnnotationLayerView(QObject *parent = nullptr);

    QObject *reader() const;
    void setReader(QObject *reader);
    QObject *settings() const;
    void setSettings(QObject *settings);
    QVariantList people() const;
    bool shown() const;
    void setShown(bool shown);
    int count() const;
    int notes() const;
    int revision() const noexcept { return m_revision; }

    Q_INVOKABLE void setPersonShown(const QString &person, bool shown);
    // On the chapter a TextEdit shows: the passages of their highlights and
    // notes, [{id, kind, person, called, color, rects: [rect]}]; and their
    // notes, where they are in its text, [{id, start, end, note, called, color, person}].
    Q_INVOKABLE QVariantList passagesIn(QQuickTextDocument *chapter) const;
    Q_INVOKABLE QVariantList notesIn(QQuickTextDocument *chapter) const;
    // Keeps the words of one of their notes as a fact of a thing of the
    // book's graph, by its number or name, made of `kind` when there is none.
    // Why not, in the window's language; empty when it was kept.
    Q_INVOKABLE QString makeFact(const QString &id, const QString &thing, const QString &kind);
    // Reads them again: a sync brought more, or the reader read on.
    Q_INVOKABLE void refresh();

    // The same as passagesIn and notesIn, of a text as it is shown, for the tests.
    struct Placed {
        const AnnotationLayers::Person *person = nullptr;
        const AnnotationLayers::Annotation *annotation = nullptr;
        qsizetype start = 0, end = 0;
    };
    [[nodiscard]] QList<Placed> placedIn(const QString &shown) const;

signals:
    void changed();

private:
    [[nodiscard]] bool isShown(const QString &person) const;

    QPointer<Reader> m_reader;
    QPointer<Settings> m_settings;
    AnnotationLayers::Layers m_layers;
    int m_revision = 0;
};
