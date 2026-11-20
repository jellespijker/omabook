#include <QQmlEngine>

#include "chronologycanvas.h"
#include "chronologymodel.h"
#include "graphcanvas.h"
#include "graphmodel.h"
#include "graphtenants.h"
#include "inksurface.h"

void registerGraphTypes() {
    // Once, however many backends are made; a static's first use is the
    // one that runs it, whichever thread gets there.
    [[maybe_unused]] static const bool registered = [] {
        qmlRegisterType<GraphBrowser>("Omabook", 1, 0, "GraphBrowser");
        qmlRegisterType<GraphCanvas>("Omabook", 1, 0, "GraphCanvas");
        qmlRegisterType<GraphMark>("Omabook", 1, 0, "GraphMark");
        qmlRegisterType<GraphTenants>("Omabook", 1, 0, "GraphTenants");
        qmlRegisterType<ChronologyBrowser>("Omabook", 1, 0, "ChronologyBrowser");
        qmlRegisterType<ChronologyCanvas>("Omabook", 1, 0, "ChronologyCanvas");
        // The ink surface, for the window and for plugins' panels (#78).
        registerInkTypes();
        return true;
    }();
}
