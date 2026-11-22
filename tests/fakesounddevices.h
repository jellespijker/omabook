#pragma once
// The desktop's sound devices for the tests (#173), which have none: a
// default output and input by id, switched as a person would.

#include <QByteArray>
#include <memory>
#include <utility>

#include "sounddevices.h"

// The desktop's devices, as the tests set them: a default output and input
// by id, changed as a person would switch them, plug in or unplug.
class FakeDevices final : public SoundDevices {
public:
    FakeDevices(QByteArray output, QByteArray input) : m_output(std::move(output)), m_input(std::move(input)) {}
    SoundDevice output() const override { return {.id = m_output, .device = {}}; }
    SoundDevice input() const override { return {.id = m_input, .device = {}}; }
    // The default is `id` now (empty: there is none); said as QMediaDevices
    // says it, also when only another device came or went.
    void setOutput(QByteArray id) {
        m_output = std::move(id);
        emit outputsChanged();
    }
    void setInput(QByteArray id) {
        m_input = std::move(id);
        emit inputsChanged();
    }

private:
    QByteArray m_output, m_input;
};

// The stand-in, kept to change it, and its owner to hand on.
struct Desktop {
    std::unique_ptr<FakeDevices> owned = std::make_unique<FakeDevices>("speakers", "built-in");
    FakeDevices *devices = owned.get();
};
