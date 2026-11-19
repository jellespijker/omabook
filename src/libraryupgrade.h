#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QString>

// Bringing a library in an older format up to date before the window opens it
// (#54). The migration and the change log's baseline (ADR 0001, Migration 1
// and 2) take a while on a large library, so they run on a worker, which opens
// a Library of its own, while a small window says so (Upgrading.qml), rather
// than the window looking frozen until they are done. Whatever goes wrong is
// said when the window then opens the library itself.
class LibraryUpgrade : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    explicit LibraryUpgrade(QObject *parent = nullptr);
    ~LibraryUpgrade() override;
    LibraryUpgrade(const LibraryUpgrade &) = delete;
    LibraryUpgrade &operator=(const LibraryUpgrade &) = delete;
    LibraryUpgrade(LibraryUpgrade &&) = delete;
    LibraryUpgrade &operator=(LibraryUpgrade &&) = delete;

    // Whether the library at `root` is in an older format than this Omabook's,
    // so that opening it brings it up to date. A new one, or none yet, isn't.
    [[nodiscard]] static bool due(const QString &root);

    // Starts bringing the library at `root` up to date when it is due, and
    // answers whether it did; finished() follows once it is done.
    bool start(const QString &root);
    [[nodiscard]] bool running() const noexcept { return m_running; }

signals:
    void runningChanged();
    void finished();

private:
    QFutureWatcher<void> m_work;
    bool m_running = false;
};
