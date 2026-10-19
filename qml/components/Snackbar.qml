pragma ComponentBehavior: Bound
import QtQuick
import Swiftify

// the error banner (ErrorBanner in the swift app) as a material snackbar: an
// error container with selectable text, an action when there is something to
// do about it, and a close button. it goes on its own after eight seconds, or
// fifteen with an action.
Item {
    id: root

    property string message
    property int serial: 0
    /// the action's label; none without one
    property string actionText
    readonly property bool shown: message.length > 0
    signal dismissed()
    signal actionTriggered()

    implicitWidth: Math.min(560, row.implicitWidth + 32)
    implicitHeight: row.implicitHeight + 24
    visible: opacity > 0
    opacity: shown ? 1 : 0
    scale: shown ? 1 : 0.94

    Behavior on opacity {
        NumberAnimation { duration: Appearance.duration(root.shown ? 200 : 150) }
    }
    Behavior on scale {
        ElementMoveSmall {}
    }

    onSerialChanged: if (shown) timer.restart()

    Timer {
        id: timer
        interval: root.actionText.length > 0 ? 15000 : 8000
        onTriggered: root.dismissed()
    }

    Rectangle {
        anchors.fill: parent
        radius: Appearance.rounding.normal
        color: Appearance.colors.errorContainer
    }

    Row {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        x: 16
        width: parent.width - 24
        spacing: 12

        ShapeIcon {
            anchors.verticalCenter: parent.verticalCenter
            size: 32
            iconSize: 18
            shape: "softBoom"
            symbol: "warning"
            color: Appearance.colors.error
            iconColor: Appearance.colors.onError
        }

        TextEdit {
            anchors.verticalCenter: parent.verticalCenter
            // the action takes its room from the text's
            width: Math.min(implicitWidth, 440 - (action.visible ? action.width + row.spacing : 0))
            text: root.message
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            color: Appearance.colors.onErrorContainer
            selectionColor: Appearance.colors.error
            selectedTextColor: Appearance.colors.onError
            font.family: Appearance.fontFamily
            font.pixelSize: Appearance.fontSize.small
        }

        TextButton {
            id: action
            anchors.verticalCenter: parent.verticalCenter
            visible: root.actionText.length > 0
            variant: TextButton.Text
            text: root.actionText
            labelColor: Appearance.colors.error
            onClicked: root.actionTriggered()
        }

        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            symbol: "close"
            size: 32
            iconSize: 20
            iconColor: Appearance.colors.onErrorContainer
            onClicked: root.dismissed()
        }
    }
}
