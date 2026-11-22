#include "scriptremedy.h"

#include <utility>

#include "modelhelp.h"
#include "settings.h"

using namespace Qt::StringLiterals;

ScriptRemedy::ScriptRemedy(Settings *settings, QObject *parent) : QObject(parent), m_settings(settings) {}

ScriptRemedy::~ScriptRemedy() = default;

ModelHelp &ScriptRemedy::help(Capability capability) {
    auto &made = m_helps[capability];
    if (!made)
        made = std::make_unique<ModelHelp>(m_settings, capability);
    return *made;
}

QVariantMap ScriptRemedy::remedy(Capability capability) {
    ModelHelp &found = help(capability);
    if (found.state().isEmpty())
        found.look();
    return found.remedy();
}

void ScriptRemedy::carryOut(Capability capability, Told told) {
    ModelHelp &found = help(capability);
    const auto connection = std::make_shared<QMetaObject::Connection>();
    // Each step as the help gets there: looked, chosen or downloading, then done.
    const auto step = [&found, connection, told = std::move(told)] {
        const QString state = found.state();
        const auto finish = [connection] { QObject::disconnect(*connection); };
        if (state == "offer"_L1) {
            found.use();
        } else if (state == "suggest"_L1) {
            found.download();
        } else if (state == "downloading"_L1) {
            if (told.progress)
                told.progress(found.progress());
        } else if (state == "chosen"_L1 || state == "there"_L1) {
            finish();
            told.done(state == "chosen"_L1 ? found.model() : found.wanted());
        } else if (state == "failed"_L1) {
            finish();
            told.failed(found.problem());
        } else if (state == "missing"_L1) {
            finish();
            told.failed(tr("Nothing on %1 can do this, and Omabook can't download a model onto it.").arg(found.server()));
        } else if (state == "none"_L1) {
            finish();
            told.failed(tr("This isn't done by a server's model here, so there is no model to find for it."));
        }
    };
    *connection = connect(&found, &ModelHelp::changed, this, step);
    const QString now = found.state();
    // What it found before may be out of date by now: it is looked at again, unless it is under way.
    if (now == "looking"_L1 || now == "downloading"_L1)
        return;
    if (now == "offer"_L1 || now == "suggest"_L1)
        step();
    else
        found.look();
}
