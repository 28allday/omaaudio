import QtQuick

// Header strip of a dock group: a row of TabButtons and an optional
// trailing action slot (the ≡ menu). Exposes currentIndex.
Rectangle {
    id: root
    property var tabs: []
    property int currentIndex: 0
    property alias trailing: trailingSlot.data
    signal activated(int index)

    implicitHeight: Theme.hDockHeader
    color: Theme.panelBg

    // Five tabs in a 320 px dock do not fit at the comfortable padding, so the
    // strip measures its labels and narrows the padding first, then, only if
    // it must, shares the width out and lets the longest labels elide.
    readonly property real available: width - Theme.s1 * 2 - trailingSlot.width
    property bool compact: false
    property real share: 1
    // Width changes arrive while a workspace is being torn down, when the
    // object's functions are already gone; a header that knows it is going
    // does nothing.
    property bool alive: true
    Component.onDestruction: alive = false
    function naturalWidth(padding) {
        var w = 0
        for (var i = 0; i < tabRepeater.count; ++i) {
            var it = tabRepeater.itemAt(i)
            if (it) w += it.labelWidth + padding * 2
        }
        return w
    }
    function relayout() {
        if (!alive || tabRepeater.count === 0 || available <= 0) return
        compact = naturalWidth(Theme.s3) > available
        var n = naturalWidth(compact ? Theme.s1 : Theme.s3)
        share = n > available && n > 0 ? available / n : 1
    }
    onAvailableChanged: relayout()
    onTabsChanged: Qt.callLater(relayout)
    Component.onCompleted: relayout()

    Row {
        anchors.left: parent.left
        anchors.leftMargin: Theme.s1
        anchors.bottom: parent.bottom
        height: parent.height
        Repeater {
            id: tabRepeater
            model: root.tabs
            TabButton {
                required property int index
                required property var modelData
                text: modelData
                uppercase: false
                height: root.height
                sidePadding: root.compact ? Theme.s1 : Theme.s3
                width: Math.ceil(implicitWidth * root.share)
                checked: root.currentIndex === index
                onClicked: { root.activated(index); if (root.currentIndex !== index) root.currentIndex = index }
                onLabelWidthChanged: Qt.callLater(root.relayout)
            }
        }
    }
    Item {
        id: trailingSlot
        anchors.right: parent.right
        anchors.rightMargin: Theme.s1
        anchors.verticalCenter: parent.verticalCenter
        width: childrenRect.width; height: parent.height
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }
}
