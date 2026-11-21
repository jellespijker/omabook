#pragma once

#include <QJSValue>
#include <QJsonObject>
#include <QObject>
#include <QString>

struct Book;
class Library;
class QJSEngine;
class ScriptPlugin;

// A book as a plugin is told of it (#137): what `omabook list --json` gives,
// (`generated` and `identifiers` always, #128) with `has_cover`, and `global_id`,
// the book's id that is the same on every computer (ADR 0001), where its
// number here is not; with `files` and `cover` too when `whole`.
namespace ScriptBook {
[[nodiscard]] QJsonObject of(const Library *library, const Book &book, bool whole = false);
} // namespace ScriptBook

// The C++ side of what a plugin writes beside a book being read (#137),
// built in scriptreading.js, which the script sees and this never is:
// `omabook.reader.selection()`, the words the reader has selected as a
// passage; `omabook.passages`, which makes a passage at a place of a book and
// finds one again in the book's text as it is now (Passage, ADR 0004); and of
// `omabook.library`, a book by its global id and the shared or published
// spaces it is in (BookSpaces).
class ScriptReading : public QObject {
    Q_OBJECT

public:
    explicit ScriptReading(ScriptPlugin *plugin) : m_plugin(plugin) {}

    // What scriptreading.js makes of this: {reader, passages, library}, for scriptprelude.js.
    [[nodiscard]] QJSValue build(QJSEngine &engine);

    // Each answers JSON, which the wrapper parses.
    Q_INVOKABLE QString selection() const;
    // `given` is a place {section, fraction}, with the words chosen there
    // (`quote`) or not, a passage, or a selection ({book, passage}).
    Q_INVOKABLE QString make(double book, const QString &given);
    Q_INVOKABLE QString find(double book, const QString &passage);
    Q_INVOKABLE QString byGlobalId(const QString &id);
    Q_INVOKABLE QString sharedIn(double book);

private:
    [[nodiscard]] Library *library() const;
    void fail(const QString &why);

    ScriptPlugin *m_plugin;
};
