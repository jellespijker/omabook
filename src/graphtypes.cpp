#include <QQmlEngine>
#include <QUrl>

#include "annotationfinder.h"
#include "annotationlayerview.h"
#include "chronologycanvas.h"
#include "chronologymodel.h"
#include "dictationkeys.h"
#include "graphcanvas.h"
#include "graphmodel.h"
#include "graphtenants.h"
#include "heldkeys.h"
#include "inksurface.h"
#include "marginnotes.h"
#include "meaningfinder.h"
#include "readingstatsview.h"

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
        // The open book's margin notes, for the reading view (#82).
        qmlRegisterType<MarginNotes>("Omabook", 1, 0, "MarginNotes");
        // What was written in the open book, found by its words, for the reader's Find (#115).
        qmlRegisterType<AnnotationFinder>("Omabook", 1, 0, "AnnotationFinder");
        // The open book found by what it means, for the reader's Find (#72).
        qmlRegisterType<MeaningFinder>("Omabook", 1, 0, "MeaningFinder");
        // The reading statistics and the year's goal (#68).
        qmlRegisterType<ReadingStatsView>("Omabook", 1, 0, "ReadingStats");
        // Others' annotations on it, as a layer beside one's own (#85).
        qmlRegisterType<AnnotationLayerView>("Omabook", 1, 0, "AnnotationLayerView");
        // Dictating into a field (#148): the window's note fields, and plugins' views.
        qmlRegisterType<DictationKeys>("Omabook", 1, 0, "DictationKeys");
        qmlRegisterType(QUrl(QStringLiteral("qrc:/DictationButton.qml")), "Omabook", 1, 0, "DictationButton");
        // A key held while a command is said (#150).
        qmlRegisterType<HeldKeys>("Omabook", 1, 0, "HeldKeys");
        return true;
    }();
}
