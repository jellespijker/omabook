#pragma once

#include <QJSValue>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QtTypes>
#include <optional>

#include "graph.h"

class Library;
class ScriptPlugin;

// The C++ side of `omabook.graph` (scriptprelude.js), beside ScriptHost
// (scriptplugin.cpp): what a plugin reads of the knowledge graph, and what
// it changes there but for the things and relations it adds, which go
// through GraphOps as the MCP tools' do and stay with the host until the
// tools' registry moves below the operations (10-architecture.md).
//
// A world (a scope of its own) is read whole; the general graph in one
// tenant (ADR 0003): the one a plugin names, else the book's it names, else
// Tenancy::preferred's (WorldLayers). What is wrong is thrown into the script.
class ScriptGraph : public QObject {
    Q_OBJECT

public:
    explicit ScriptGraph(ScriptPlugin *plugin) : m_plugin(plugin) {}

    // Where in a book `at` ({section, fraction}) says, or none without a book or a section.
    [[nodiscard]] static std::optional<GraphPlace> place(qint64 bookId, const QJsonObject &at);

    // `options` is {kind, scope, everywhere, limit, tenant, book}, as JSON.
    Q_INVOKABLE QString find(const QString &words, const QString &options);
    Q_INVOKABLE QString node(double id);
    Q_INVOKABLE QString around(double id, double depth);
    Q_INVOKABLE QString ofBook(double id, const QString &upTo);
    Q_INVOKABLE QString beyond(double id, const QString &upTo);
    // One request to the model the person chose: what a book connects to in
    // the other books read. Resolves with how many connections it added.
    Q_INVOKABLE void connectBook(double id, const QJSValue &resolve, const QJSValue &reject);
    Q_INVOKABLE QString whole(const QString &scope, double limit);
    // The node that stands for a book (Graph::bookNode), made when it isn't
    // there yet: its number, or 0 (thrown) for a book the library hasn't.
    Q_INVOKABLE double bookNode(double id);
    // The timeline of a book's own story (Chronology::timelineFor), made when
    // it isn't there yet: its number, or 0 (thrown) for a book the library hasn't.
    Q_INVOKABLE double timeline(double book);
    // A timeline that branches off another at an event (#171,
    // chronologybranch.h): `options` is {timeline, name, from, at, book,
    // section, fraction}, as JSON. Its number; what is wrong is thrown.
    Q_INVOKABLE double branch(const QString &options);
    // Takes away where a thing was met in a book (Graph::unsee); false when it wasn't.
    Q_INVOKABLE bool unsee(double id, double book);
    Q_INVOKABLE bool unlink(double edge);
    Q_INVOKABLE bool merge(double keep, double other);
    Q_INVOKABLE bool remove(double id);
    Q_INVOKABLE QString scopes();
    Q_INVOKABLE bool copyScope(const QString &from, const QString &to);
    Q_INVOKABLE bool removeScope(const QString &scope);

private:
    // The library open now; with `changing`, none is thrown into the script.
    [[nodiscard]] Library *library(bool changing);
    void fail(const QString &why);

    ScriptPlugin *m_plugin;
};
