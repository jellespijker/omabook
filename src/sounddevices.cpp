#include "sounddevices.h"

#include <QMediaDevices>

namespace {

[[nodiscard]] SoundDevice known(const QAudioDevice &device) { return {.id = device.id(), .device = device}; }

class SystemDevices final : public SoundDevices {
public:
    SystemDevices() {
        // QMediaDevices says so for a new default as well as for a device
        // added or gone.
        connect(&m_devices, &QMediaDevices::audioOutputsChanged, this, &SoundDevices::outputsChanged);
        connect(&m_devices, &QMediaDevices::audioInputsChanged, this, &SoundDevices::inputsChanged);
    }
    [[nodiscard]] SoundDevice output() const override { return known(QMediaDevices::defaultAudioOutput()); }
    [[nodiscard]] SoundDevice input() const override { return known(QMediaDevices::defaultAudioInput()); }

private:
    QMediaDevices m_devices;
};

} // namespace

std::unique_ptr<SoundDevices> SoundDevices::system() { return std::make_unique<SystemDevices>(); }

DefaultDevice::DefaultDevice(Way way, std::unique_ptr<SoundDevices> devices, QObject *parent)
    : QObject(parent), m_way(way), m_devices(std::move(devices)) {
    m_now = m_way == Way::Out ? m_devices->output() : m_devices->input();
    connect(m_devices.get(), m_way == Way::Out ? &SoundDevices::outputsChanged : &SoundDevices::inputsChanged, this, &DefaultDevice::check);
}

void DefaultDevice::check() {
    SoundDevice now = m_way == Way::Out ? m_devices->output() : m_devices->input();
    if (now == m_now)
        return;
    m_now = std::move(now);
    if (!m_now.id.isEmpty())
        emit moved(m_now);
}
