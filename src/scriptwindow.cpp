#include "scriptwindow.h"

#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <algorithm>

#include "scriptplugin.h"

using namespace Qt::StringLiterals;

QJSValue ScriptWindow::build(QJSEngine &engine) {
    QFile bundled(u":/scriptui.js"_s);
    if (!bundled.open(QIODevice::ReadOnly))
        return engine.newErrorObject(QJSValue::GenericError, u"Omabook's own side of omabook.ui is missing."_s);
    QJSEngine::setObjectOwnership(this, QJSEngine::CppOwnership);
    return engine.evaluate(QString::fromUtf8(bundled.readAll()), u"omabook-api/ui"_s).call({engine.newQObject(this)});
}

void ScriptWindow::fail(const QString &why) { m_plugin->m_engine.throwError(why); }

QString ScriptWindow::source(const QString &file, const QString &what) {
    QString path = m_plugin->inside(file, false);
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        fail(u"There is no \"%1\" in the plugin's folder for the %2."_s.arg(file, what));
        return {};
    }
    return path;
}

void ScriptWindow::registerPanel(const QString &place, const QString &title, int width, const QString &source, const QString &when) {
    const QString path = this->source(source, u"panel"_s);
    if (path.isEmpty())
        return;
    // The same file again is the same panel, said anew.
    m_plugin->m_panels.removeIf([&path](const ScriptPlugin::Panel &panel) { return panel.view.isEmpty() && panel.source == path; });
    m_plugin->m_panels.append({place, title, path, std::clamp(width, 120, 900), when, true, {}});
    emit m_plugin->registered();
}

void ScriptWindow::setPanelVisible(const QString &source, bool visible) {
    bool found = false;
    for (ScriptPlugin::Panel &panel : m_plugin->m_panels) {
        if (panel.view.isEmpty() && QFileInfo(panel.source).fileName() == QFileInfo(source).fileName()) {
            panel.visible = visible;
            found = true;
        }
    }
    if (found)
        emit m_plugin->registered();
    else
        fail(u"There is no panel \"%1\" of this plugin's to show or hide."_s.arg(source));
}

void ScriptWindow::registerView(const QString &id, const QString &title, int width, const QString &source) {
    const QString path = this->source(source, u"view"_s);
    if (path.isEmpty())
        return;
    m_plugin->m_panels.removeIf([&id](const ScriptPlugin::Panel &panel) { return panel.view == id; });
    // Its width is the one it comes with beside the page; in place of the shelf it has the window.
    m_plugin->m_panels.append({u"view"_s, title.simplified(), path, std::clamp(width, 280, 900), u"always"_s, true, id});
    emit m_plugin->registered();
}

bool ScriptWindow::knowsView(const QString &id) {
    if (std::ranges::any_of(m_plugin->m_panels, [&id](const ScriptPlugin::Panel &panel) { return panel.view == id; }))
        return true;
    fail(u"There is no view \"%1\" of this plugin's: give it to omabook.ui.view first."_s.arg(id));
    return false;
}

void ScriptWindow::showView(const QString &id, const QString &args, bool toggle) {
    if (knowsView(id))
        emit m_plugin->viewRequested(id, QJsonDocument::fromJson(args.toUtf8()).object(), toggle);
}

void ScriptWindow::closeView(const QString &id) {
    if (knowsView(id))
        emit m_plugin->viewClosed(id);
}

void ScriptWindow::registerLauncher(const QString &launcher, const QJSValue &open, const QJSValue &removed) {
    const QJsonObject said = QJsonDocument::fromJson(launcher.toUtf8()).object();
    const QString id = said.value("id"_L1).toString();
    const QString cover = said.value("cover"_L1).toString();
    QString picture;
    if (!cover.isEmpty()) {
        picture = m_plugin->inside(cover, false);
        if (picture.isEmpty() || !QFileInfo(picture).isFile()) {
            fail(u"There is no \"%1\" in the plugin's folder for the launcher's cover."_s.arg(cover));
            return;
        }
    }
    // The same id again is the same launcher, said anew; a plugin has
    // few, since each is a place on everybody's shelf.
    QList<ScriptPlugin::Launcher> &launchers = m_plugin->m_launchers;
    launchers.removeIf([&id](const ScriptPlugin::Launcher &kept) { return kept.id == id; });
    if (launchers.size() >= 3) {
        fail(u"A plugin puts at most three launchers on the shelf."_s);
        return;
    }
    launchers.append({id, said.value("title"_L1).toString().simplified(), said.value("author"_L1).toString().simplified(),
                      said.value("description"_L1).toString().trimmed(), picture, open, removed});
    emit m_plugin->registered();
}

void ScriptWindow::removeLauncher(const QString &id) {
    if (m_plugin->m_launchers.removeIf([&id](const ScriptPlugin::Launcher &launcher) { return launcher.id == id; }) > 0)
        emit m_plugin->registered();
}

void ScriptWindow::notify(const QString &text, const QString &action, const QJSValue &run) {
    const QString said = text.simplified();
    if (said.isEmpty())
        return;
    static const QMetaMethod shown = QMetaMethod::fromSignal(&ScriptPlugin::notified);
    if (!m_plugin->isSignalConnected(shown)) {
        emit m_plugin->said(said);
        return;
    }
    const bool acts = run.isCallable() && !action.simplified().isEmpty();
    const int token = acts ? m_plugin->m_nextToken++ : 0;
    if (acts) {
        // A notice is soon gone: only the last few can still be acted on.
        m_actions.removeIf([token](QHash<int, QJSValue>::iterator kept) { return kept.key() < token - 8; });
        m_actions.insert(token, run);
    }
    emit m_plugin->notified(said, acts ? action.simplified() : QString(), token);
}

void ScriptWindow::act(int token) {
    const QJSValue run = m_actions.take(token);
    if (run.isCallable())
        m_plugin->invoke(run, QJsonValue(), [this](const QJsonValue &, const QString &problem) {
            if (!problem.isEmpty())
                m_plugin->report(problem);
        });
}
