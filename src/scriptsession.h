#pragma once

#include <QNetworkAccessManager>
#include <QString>
#include <memory>
#include <optional>

#include "audiodevices.h"
#include "library.h"
#include "scriptplugin.h"
#include "scripttools.h"
#include "settings.h"

class QCommandLineParser;

// What a call of ScriptTools works with: the settings, the plugin folder and
// the library, each the caller's when it brought one and the user's
// otherwise, and the sound a plugin records and plays (a test's, #135). The
// library is only opened when a script asks for it.
class ScriptSession {
public:
    explicit ScriptSession(ScriptTools::Context context);

    [[nodiscard]] QString folder() const { return m_context.folder; }
    [[nodiscard]] Settings *settings() const { return m_context.settings; }
    Library *library();
    [[nodiscard]] ScriptServices services();

    // Whether the library in `folder` is a scratch one, where a plugin may be
    // tried with a test microphone and a yes to recording given beforehand
    // (#135): none of the person's own libraries, those the settings name.
    [[nodiscard]] static bool scratch(const Settings &settings, const QString &folder);

    // The library a command was given (--library), and the sound `run` tries a
    // plugin with (#135): a test microphone and a yes to recording given
    // beforehand, never on the person's own library, since what is recorded there
    // goes to their other computers. False, with why said, when it can't be.
    [[nodiscard]] static bool asAsked(const QCommandLineParser &parser, ScriptTools::Context &context, std::unique_ptr<Library> &library,
                                      std::optional<AudioDevices> &room);

private:
    ScriptTools::Context m_context;
    std::unique_ptr<Settings> m_ownSettings;
    std::unique_ptr<Library> m_ownLibrary;
    bool m_libraryTried = false;
    QNetworkAccessManager m_network;
};
