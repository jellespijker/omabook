#pragma once

#include <QCommandLineParser>
#include <QString>
#include <expected>

#include "graph.h"
#include "library.h"

// The graph an `omabook graph` command reads or writes in (ADR 0003): that of
// the tenant named (--collection), of the book or the thing it names, or the
// library's only one; a world (--scope) whole (Graph::ofScope, #112); and for
// `scopes` with nothing named, those of every tenant. Otherwise why not,
// naming the tenants there are, so that the caller asks rather than mixes.
namespace GraphChoice {

struct Chosen {
    Graph graph;
    bool everyTenant = false; // `scopes`, of every tenant (GraphOps::everyScope)
};

[[nodiscard]] std::expected<Chosen, QString> of(const Library &library, const QCommandLineParser &parser, const QString &sub, qint64 first);

} // namespace GraphChoice
