#pragma once

#include <QString>
#include <QStringList>

class QCommandLineParser;

// The links between a person's computers and their hub (#180, ADR 0012), as
// `omabook hub` takes them. On a computer: `add <host>` links this computer
// (--name: what the hub calls it), `remove <host>` takes it off, and
// `computers <host>` lists the hub's computers. On the hub: `computers` lists
// them, `forget <computer>` takes one's key away, and `allow`, which `add`
// runs there over ssh, installs a computer's key read from its input.
// `what` is the word after `hub`; `words` what follows it.
[[nodiscard]] bool isHubLinkCommand(const QString &what);
int runHubLink(const QString &what, const QStringList &words, const QCommandLineParser &parser);
