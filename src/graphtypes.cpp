#include <QQmlEngine>

#include "chronologycanvas.h"
#include "chronologymodel.h"
#include "graphcanvas.h"
#include "graphmodel.h"

void registerGraphTypes() {
    // Once, however many backends are made; a static's first use is the
    // one that runs it, whichever thread gets there.
    [[maybe_unused]] static const bool registered = [] {
        qmlRegisterType<GraphBrowser>("Omabook", 1, 0, "GraphBrowser");
        qmlRegisterType<GraphCanvas>("Omabook", 1, 0, "GraphCanvas");
        qmlRegisterType<GraphMark>("Omabook", 1, 0, "GraphMark");
        qmlRegisterType<ChronologyBrowser>("Omabook", 1, 0, "ChronologyBrowser");
        qmlRegisterType<ChronologyCanvas>("Omabook", 1, 0, "ChronologyCanvas");
        return true;
    }();
}
