#pragma once

#include <QQuickPaintedItem>
#include <QString>
#include <optional>

#include "qrcode.h"

// A QR code on screen (#177), for a phone's camera: always dark on white,
// whatever the theme, with the light margin around it that a reader looks
// for, in whole pixels a module so the camera sees sharp edges. Empty when
// there is no text, or too much of it.
class QrView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed)
    // Whether there is a code to show.
    Q_PROPERTY(bool valid READ valid NOTIFY changed)
    // Modules on a side, the margin included.
    Q_PROPERTY(int modules READ modules NOTIFY changed)

public:
    // The light modules around the code on each side.
    static constexpr int margin = 4;

    explicit QrView(QQuickItem *parent = nullptr);

    [[nodiscard]] QString text() const { return m_text; }
    void setText(const QString &text);
    [[nodiscard]] bool valid() const noexcept { return m_code.has_value(); }
    [[nodiscard]] int modules() const { return m_code ? m_code->size() + 2 * margin : 0; }

    void paint(QPainter *painter) override;

signals:
    void changed();

private:
    QString m_text;
    std::optional<QrCode> m_code;
};
