#pragma once

#include <QNetworkAccessManager>
#include <QString>
#include <memory>

#include "library.h"
#include "scriptplugin.h"
#include "scripttools.h"
#include "settings.h"

// What a call of ScriptTools works with: the settings, the plugin folder and
// the library, each the caller's when it brought one and the user's
// otherwise, and the sound a plugin records and plays (a test's, #135). The
// library is only opened when a script asks for it.
class ScriptSession {
public:
    explicit ScriptSession(const ScriptTools::Context &context);

    [[nodiscard]] QString folder() const { return m_context.folder; }
    [[nodiscard]] Settings *settings() const { return m_context.settings; }
    Library *library();
    [[nodiscard]] ScriptServices services();

    // Whether the library in `folder` is a scratch one, where a plugin may be
    // tried with a test microphone and a yes to recording given beforehand
    // (#135): none of the person's own libraries, those the settings name.
    [[nodiscard]] static bool scratch(const Settings &settings, const QString &folder);

private:
    ScriptTools::Context m_context;
    std::unique_ptr<Settings> m_ownSettings;
    std::unique_ptr<Library> m_ownLibrary;
    bool m_libraryTried = false;
    QNetworkAccessManager m_network;
};
