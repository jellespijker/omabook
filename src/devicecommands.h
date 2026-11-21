#pragma once

#include <QStringList>

// The commands of e-readers and phones (#71): `serve` (the OPDS catalog and
// KOReader's progress sync, for as long as it runs), `koreader` (log in to a
// KOReader sync server, sync the reading places with it, or a file's
// document hash), `send` (a book to the mounted e-reader, or by mail to a
// Kindle), `convert` (a book to another format, kept with it, as sending
// does when a device needs one) and `opds` (browse a catalog, and add a book
// from it).
int runServeCli(const QStringList &arguments);
int runKoreaderCli(const QStringList &arguments);
int runSendCli(const QStringList &arguments);
int runConvertCli(const QStringList &arguments);
int runOpdsCli(const QStringList &arguments);
