#pragma once

#include <QObject>
#include <QString>
#include <functional>

class Library;

// The C++ side of `omabook.log.set`, `add`, `remove`, `read` and `list`
// (#76): a script plugin's own data in the change log (PluginData), under
// its id and no other's, so a plugin reads and writes its own things alone.
// What is wrong with a write is thrown into the script, and nothing of it is
// written.
class ScriptChangeLog : public QObject {
    Q_OBJECT

public:
    // The library open now (none, with an error thrown, when there is none),
    // and how an error is thrown into the script.
    using LibraryOf = std::function<Library *()>;
    using Fail = std::function<void(const QString &)>;

    ScriptChangeLog(QString plugin, LibraryOf library, Fail fail);

    // Each of what is JSON here comes from the prelude, which made it with JSON.stringify.
    Q_INVOKABLE void set(const QString &entity, const QString &id, const QString &fields);
    Q_INVOKABLE void add(const QString &entity, const QString &id, const QString &set, const QString &items, const QString &removed);
    // The thing itself without a set, else the keys of the set's items.
    Q_INVOKABLE void remove(const QString &entity, const QString &id, const QString &set, const QString &keys);
    Q_INVOKABLE QString read(const QString &entity, const QString &id);
    Q_INVOKABLE QString list(const QString &entity);

private:
    QString m_plugin;
    LibraryOf m_library;
    Fail m_fail;
};
