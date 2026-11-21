#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QString>
#include <QVariantList>
#include <vector>

#include "annotationlayers.h"
#include "inkstroke.h"

class InkSurface;
class Reader;
class Settings;

// Others' annotations on the open book (#85), for the reading view: each
// person's highlights and margin notes as far as the reader has read
// (AnnotationLayers), in that person's colour and under their name, beside
// the reader's own and never among them. They are switched off all at once,
// or one person at a time, in the settings (Settings::othersAnnotations,
// hiddenAnnotators), which every computer of the reader's keeps for itself.
//
// Their book ink (#83) is drawn over the pages of a PDF or a comic, and the
// ink of a margin note they wrote by hand on its card, each by a read-only
// ink surface, in their colour, no more strokes of an annotation than
// AnnotationLayers reads (PluginData::maxItems).
//
// Their words are shown as plain text (UntrustedText::plain), never as
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
    // Ink units across a handwritten note's card (MarginNotes::inkUnits).
    Q_PROPERTY(double noteInkUnits READ noteInkUnits CONSTANT)

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
    double noteInkUnits() const;

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
    // Their ink on a page of the open book, drawn by `surface` (a read-only
    // InkSurface over it) until it is forgotten or goes.
    Q_INVOKABLE void showInk(QQuickItem *surface, int page);
    // The ink of their handwritten note `id`, drawn by `surface` on its card the same way.
    Q_INVOKABLE void showNoteInk(QQuickItem *surface, const QString &id);
    Q_INVOKABLE void forgetInk(QQuickItem *surface);
    // How far down a surface's strokes reach, in ink units.
    Q_INVOKABLE double inkBottom(QQuickItem *surface) const;
    // Reads them again: a sync brought more, or the reader read on.
    Q_INVOKABLE void refresh();

    // The strokes shown of others' ink on a page, or of a note by its id,
    // each in its person's colour, in the order they were drawn.
    [[nodiscard]] std::vector<Ink::Stroke> inkOn(int page) const;
    [[nodiscard]] std::vector<Ink::Stroke> noteInk(const QString &id) const;

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
    // What an ink surface draws: of a page, or of a note.
    struct Drawn {
        int page = -1;
        QString note;
        QPointer<InkSurface> surface;
    };
    [[nodiscard]] std::vector<Ink::Stroke> strokesOf(const Drawn &drawn) const;
    void redraw();

    QPointer<Reader> m_reader;
    QPointer<Settings> m_settings;
    AnnotationLayers::Layers m_layers;
    QList<Drawn> m_drawn;
    int m_revision = 0;
};
