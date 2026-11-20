import QtQuick
import Omabook

// A page's book ink, small, in the list beside the page (#83): the part of
// the page it covers, drawn with the ink on it as the page shows it.
Item {
    id: thumb
    // {page, x, y, width, height}: that part, measured in the page's width (PageRegion).
    property var region: ({ page: 0, x: 0, y: 0, width: 1, height: 1 })
    clip: true
    // The whole page's width in pixels, so that the part fills the picture.
    readonly property real across: Math.min(900, Math.max(width, Math.min(width / Math.max(0.02, region.width),
                                                                          height / Math.max(0.02, region.height))))
    readonly property real aspect: {
        const known = reader.pageAspect(region.page)
        if (known > 0) return known
        return picture.implicitWidth > 0 ? picture.implicitHeight / picture.implicitWidth : 1.414
    }

    Rectangle { anchors.fill: parent; color: "white" }
    Item {
        x: (thumb.width - thumb.region.width * thumb.across) / 2 - thumb.region.x * thumb.across
        y: (thumb.height - thumb.region.height * thumb.across) / 2 - thumb.region.y * thumb.across
        width: thumb.across
        height: thumb.across * thumb.aspect
        Image {
            id: picture
            anchors.fill: parent
            asynchronous: true
            fillMode: Image.PreserveAspectFit
            source: reader.imagePrefix + "page/" + thumb.region.page + "/" + Math.round(thumb.across * Screen.devicePixelRatio)
        }
        InkSurface {
            id: ink
            objectName: "inkThumb" + thumb.region.page
            anchors.fill: parent
            readOnly: true
            inkScale: Math.max(0.01, width / reader.ink.pageUnits)
            Component.onCompleted: reader.ink.show(ink, thumb.region.page)
            Component.onDestruction: reader.ink.forget(ink)
        }
    }
}
