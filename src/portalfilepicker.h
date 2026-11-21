#pragma once

#include <QVariantMap>

#include "filepicker.h"

class QDBusPendingCallWatcher;

// The desktop's own open and save dialogs, through xdg-desktop-portal.
class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void pick(Purpose purpose, const QString &suggested = {}) override;
    void pickForPlugin(const QStringList &kinds, bool multiple) override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    void requestFile(const QString &method, const QString &title, QVariantMap options, Purpose purpose);
    bool connectToRequestPath(const QString &path);
    void clearPending();
    // Best-effort Close of the pending portal request, if there is one.
    void closePendingRequest();

    QString m_pendingPath;
    bool m_pending = false;
    Purpose m_pendingPurpose = AddBooks;
    // Bumped per request, so a late reply for a replaced request is ignored.
    int m_pendingGeneration = 0;
    QDBusPendingCallWatcher *m_pendingWatcher = nullptr;
};
