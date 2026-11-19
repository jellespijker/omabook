#pragma once

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QtTypes>
#include <utility>

#include "graph.h"

class Library;

// The knowledge graphs of the spaces this library subscribes to (ADR 0001,
// *Subscribing*; #5), each a layer beside the reader's own graph and never in
// it: read from the space's operations (SubscribedSpace), never from the
// graph's rows, so nothing of it is written, built on or synced as one's own.
//
// A layer holds only what the reader knows, as the predicate it is read with
// says. Everything in it has a place that predicate is asked about: a thing
// is in it only as met at a known place, a fact or a relation only as learned
// at one. So what the owner put in by hand, of no book, has no place to be
// cut at and stays out, as does all of a book the reader doesn't have here.
// The cut every surface asks for is GraphContext::layers, by the reader's own
// place in each book; read() is how it is made.
//
// Its things and relations have ids of their own, below zero and the same
// each time they are read, so they never meet one's own. Each thing says
// where it is from: its scope is the space's id, and its data's "layer" the
// space's name and its owner's (from()).
namespace GraphLayers {

struct Layer {
    QString space; // "published:…"
    QString name;  // the space's
    QString owner; // as its owner chose to be called
    // Each thing as it is before what is known of it: no summary, and only the
    // names it was given at a known place. Graph::asKnown with its facts
    // says what is known of it in a book (metIn and ofBook do).
    QList<GraphNode> nodes;
    QHash<qint64, QList<GraphFact>> facts; // what is known of each thing, by thing
    QHash<qint64, QList<GraphMoment>> met; // where each thing was met, once per book
    QList<GraphEdge> edges;

    // The things met in a book, each as known there, in the order met (as
    // Graph::metIn answers the reader's own).
    [[nodiscard]] QList<std::pair<GraphNode, GraphPlace>> metIn(qint64 bookId) const;
    // A book's part: those things and the relations among them.
    [[nodiscard]] GraphView ofBook(qint64 bookId) const;
    // Where each thing is tied to a book, as Graph::sightings says of one's own.
    [[nodiscard]] QList<Graph::Sighting> sightings() const;
};

// Of every space this library subscribes to, a layer of what `known` says is
// known (a place of no book never is); one for each, also when nothing of it is.
[[nodiscard]] QList<Layer> read(const Library &library, const GraphKnown &known);

// How `from` says it: in the language the window is in, or in English, as
// the AI is told it.
enum class Said { Shown, English };
// Where a thing is from: "Ada's shared graph" for a thing of a layer, empty
// for one's own.
[[nodiscard]] QString from(const GraphNode &node, Said said = Said::Shown);

// A view of one's own of a book, with the layers' part of it after.
[[nodiscard]] GraphView with(GraphView view, const QList<Layer> &layers, qint64 bookId);
// Adds the things the layers met in a book, and their facts, to those of
// one's own (as Graph::metIn and Graph::factsOf answer them), in the order met.
void addTo(QList<std::pair<GraphNode, GraphPlace>> &met, QHash<qint64, QList<GraphFact>> &facts, const QList<Layer> &layers, qint64 bookId);

// The layers' part of a book: [{space, name, owner, nodes, edges}].
[[nodiscard]] QJsonArray toJson(const QList<Layer> &layers, qint64 bookId);

} // namespace GraphLayers
