#include "modelhelp.h"

#include <QLocale>
#include <algorithm>
#include <iterator>

#include "settings.h"

using namespace Qt::StringLiterals;

ModelHelp::ModelHelp(QObject *parent) : QObject(parent) {}

ModelHelp::ModelHelp(Settings *settings, Capability capability, QObject *parent)
    : QObject(parent), m_settings(settings), m_capability(capability) {}

QObject *ModelHelp::settings() const { return m_settings; }

void ModelHelp::setSettings(QObject *settings) {
    auto *given = qobject_cast<Settings *>(settings);
    if (given == m_settings)
        return;
    m_settings = given;
    emit settingsChanged();
}

QString ModelHelp::capability() const { return m_capability ? Capabilities::name(*m_capability) : QString(); }

void ModelHelp::setCapability(const QString &name) {
    const auto named = Capabilities::named(name);
    if (named == m_capability)
        return;
    cancel();
    m_capability = named;
    setState({});
    emit settingsChanged();
}

QString ModelHelp::size() const { return m_bytes > 0 ? QLocale().formattedDataSize(m_bytes, 1, QLocale::DataSizeSIFormat) : QString(); }

void ModelHelp::setState(const QString &state) {
    m_state = state;
    emit changed();
}

void ModelHelp::look() {
    if (!m_settings || !m_capability)
        return;
    const Capability capability = *m_capability;
    m_scout.cancel();
    m_place = ModelFit::placeOf(*m_settings, capability);
    m_wanted = m_place ? m_place->model : QString();
    m_model.clear();
    m_problem.clear();
    m_fitting.clear();
    m_bytes = 0;
    m_unfit = false;
    if (!m_place) {
        setState(u"none"_s);
        return;
    }
    const AiServer server = m_place->server;
    setState(u"looking"_s);
    m_scout.list(server, [this, capability](const auto &listing) { listed(capability, listing); });
}

void ModelHelp::listed(Capability capability, const std::expected<ModelScout::Listing, Failure> &listing) {
    if (!listing) {
        m_problem = listing.error().text();
        setState(u"failed"_s);
        return;
    }
    // The model asked for is there and can do it, or it can't be told: nothing to mend.
    const auto there =
        std::ranges::find_if(listing->models, [this](const ModelFit::Model &one) { return ModelFit::same(one.name, m_wanted); });
    m_unfit = there != listing->models.end() && ModelFit::fit(capability, *there) == ModelFit::Fit::Unfit;
    if (there != listing->models.end() && !m_unfit) {
        setState(u"there"_s);
        return;
    }
    QList<ModelFit::Model> fits;
    std::ranges::copy_if(listing->models, std::back_inserter(fits),
                         [capability](const ModelFit::Model &one) { return ModelFit::fit(capability, one) == ModelFit::Fit::Fits; });
    // Quick work wants the smallest; the rest the one the server lists first.
    if (capability == Capability::Quick)
        std::ranges::stable_sort(fits, {}, &ModelFit::Model::bytes);
    for (const ModelFit::Model &one : std::as_const(fits))
        m_fitting.append(one.name);
    if (!m_fitting.isEmpty()) {
        m_model = m_fitting.first();
        setState(u"offer"_s);
        return;
    }
    const auto suggestion = ModelFit::suggested(capability);
    if (listing->ollama && suggestion) {
        m_model = suggestion->name;
        m_bytes = suggestion->bytes;
        setState(u"suggest"_s);
        return;
    }
    setState(u"missing"_s);
}

void ModelHelp::use(const QString &model) {
    const QString picked = model.trimmed().isEmpty() ? m_model : model.trimmed();
    if (!m_settings || !m_capability || picked.isEmpty())
        return;
    ModelFit::choose(*m_settings, *m_capability, picked);
    m_model = picked;
    m_wanted = picked;
    setState(u"chosen"_s);
    emit chosen(picked);
}

void ModelHelp::download() {
    // Once it failed, it may be tried again.
    const bool offered = m_state == "suggest"_L1 || (m_state == "failed"_L1 && m_bytes > 0);
    if (!offered || !m_place)
        return;
    const AiServer server = m_place->server;
    m_progress = 0;
    setState(u"downloading"_s);
    m_scout.pull(
        server, m_model,
        [this](qint64 done, qint64 total) {
            m_progress = std::clamp(static_cast<double>(done) / static_cast<double>(total), 0.0, 1.0);
            emit changed();
        },
        [this](const std::expected<void, Failure> &pulled) {
            if (!pulled) {
                m_problem = pulled.error().text();
                setState(u"failed"_s);
                return;
            }
            use(m_model);
        });
}

void ModelHelp::cancel() {
    m_scout.cancel();
    if (m_state == "downloading"_L1)
        setState(u"suggest"_s);
    else if (m_state == "looking"_L1)
        setState({});
}

QVariantMap ModelHelp::remedy() const {
    if (m_state != "offer"_L1 && m_state != "suggest"_L1)
        return {};
    return {{u"action"_s, m_state == "offer"_L1 ? u"use"_s : u"download"_s},
            {u"model"_s, m_model},
            {u"size"_s, m_bytes},
            {u"server"_s, server()}};
}
