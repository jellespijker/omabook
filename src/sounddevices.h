#pragma once

#include <QAudioDevice>
#include <QByteArray>
#include <QObject>
#include <memory>

// A sound device the desktop has, known by its id. The tests' stand-in for
// the desktop has ids only: there, `device` is null.
struct SoundDevice {
    QByteArray id;
    QAudioDevice device;

    friend bool operator==(const SoundDevice &one, const SoundDevice &other) { return one.id == other.id; }
};
Q_DECLARE_METATYPE(SoundDevice)

// Which output and which input the desktop uses by default, and word when
// that, or the list it is chosen from, may have changed (#173): QMediaDevices
// in the app (on Omarchy, PipeWire's default), a stand-in in the tests, which
// have no sound card.
class SoundDevices : public QObject {
    Q_OBJECT

public:
    // None (an empty id) when the desktop has none.
    [[nodiscard]] virtual SoundDevice output() const = 0;
    [[nodiscard]] virtual SoundDevice input() const = 0;

    // The desktop's own.
    [[nodiscard]] static std::unique_ptr<SoundDevices> system();

signals:
    void outputsChanged();
    void inputsChanged();
};

// One way of sound kept on the desktop's default device as it changes
// (#173): headphones plugged in, Bluetooth connected, the output switched in
// the desktop's settings, or the device in use gone. Omabook has no choice of
// device in its own Settings, so the default is always the one.
class DefaultDevice : public QObject {
    Q_OBJECT

public:
    enum class Way { Out, In };

    explicit DefaultDevice(Way way, std::unique_ptr<SoundDevices> devices = SoundDevices::system(), QObject *parent = nullptr);

    // The default when it was made or last moved.
    [[nodiscard]] const SoundDevice &now() const { return m_now; }

signals:
    // The default is another device now, `to`: what plays or listens moves
    // there. Not said when the desktop has none left; then what is in use
    // stays where it is, and fails as that device goes.
    void moved(const SoundDevice &to);

private:
    void check();

    Way m_way;
    std::unique_ptr<SoundDevices> m_devices;
    SoundDevice m_now;
};
