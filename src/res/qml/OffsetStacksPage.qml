import QtQuick 2.7
import QtQuick.Controls 2.0
import QtQuick.Layouts 1.3
import ovras.advsettings 1.0
import "." // QTBUG-34418, singletons require explicit import to load qmldir file
import "common"

// Lists offsets contributed by other software over IPC (docs/ipc).
// The playspace is placed at: your offset + all stacks.
MyStackViewPage {
    id: offsetStacksPage
    headerText: "Offset Stacks"

    property var stacks: []
    property var total: ({ x: 0, y: 0, z: 0, rotation: 0 })

    function fmt(v, digits) {
        return Number(v).toFixed(digits)
    }

    function refresh() {
        var newStacks = MoveCenterTabController.offsetStackList()
        // Only rebuild rows when something actually changed.
        if (JSON.stringify(newStacks) !== JSON.stringify(stacks)) {
            stacks = newStacks
        }
        total = MoveCenterTabController.totalOffsetMap()
    }

    // Stacks can change every frame (e.g. a leash); poll while visible
    // instead of reacting to every change.
    Timer {
        interval: 200
        repeat: true
        running: offsetStacksPage.visible
        triggeredOnStart: true
        onTriggered: offsetStacksPage.refresh()
    }

    Connections {
        target: MoveCenterTabController
        onOffsetStacksChanged: offsetStacksPage.refresh()
    }

    content: ColumnLayout {
        spacing: 18

        MyText {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Offsets added by other software through the IPC server. They are added on top of your own offset, which Space Drag, Space Turn and Reset change. Resetting your offset keeps these."
        }

        RowLayout {
            spacing: 18
            MyText {
                text: "Applied total:"
                Layout.preferredWidth: 230
            }
            MyText {
                text: "X " + fmt(total.x, 2) + "   Y " + fmt(total.y, 2)
                      + "   Z " + fmt(total.z, 2) + "   Rot " + fmt(total.rotation, 1) + "°"
            }
        }

        // Column headers
        RowLayout {
            spacing: 18
            visible: stacks.length > 0
            MyText { text: "Stack"; Layout.preferredWidth: 300; font.bold: true }
            MyText { text: "X"; Layout.preferredWidth: 110; font.bold: true; horizontalAlignment: Text.AlignRight }
            MyText { text: "Y"; Layout.preferredWidth: 110; font.bold: true; horizontalAlignment: Text.AlignRight }
            MyText { text: "Z"; Layout.preferredWidth: 110; font.bold: true; horizontalAlignment: Text.AlignRight }
            MyText { text: "Rot"; Layout.preferredWidth: 110; font.bold: true; horizontalAlignment: Text.AlignRight }
            MyText { text: "Lifetime"; Layout.preferredWidth: 170; font.bold: true }
        }

        Repeater {
            model: offsetStacksPage.stacks
            delegate: RowLayout {
                spacing: 18
                MyText {
                    text: modelData.id
                    Layout.preferredWidth: 300
                    elide: Text.ElideRight
                }
                MyText { text: fmt(modelData.x, 2); Layout.preferredWidth: 110; horizontalAlignment: Text.AlignRight }
                MyText { text: fmt(modelData.y, 2); Layout.preferredWidth: 110; horizontalAlignment: Text.AlignRight }
                MyText { text: fmt(modelData.z, 2); Layout.preferredWidth: 110; horizontalAlignment: Text.AlignRight }
                MyText { text: fmt(modelData.rotation, 1) + "°"; Layout.preferredWidth: 110; horizontalAlignment: Text.AlignRight }
                MyText {
                    text: modelData.persist ? "Persistent" : "Connection"
                    Layout.preferredWidth: 170
                }
                MyPushButton {
                    text: "Reset"
                    Layout.preferredWidth: 150
                    onClicked: {
                        MoveCenterTabController.resetOffsetStack(modelData.id)
                        offsetStacksPage.refresh()
                    }
                }
            }
        }

        MyText {
            visible: stacks.length === 0
            text: "No software offsets are active."
            opacity: 0.7
        }

        Item { Layout.fillHeight: true; Layout.fillWidth: true }

        RowLayout {
            Layout.fillWidth: true
            MyPushButton {
                text: "Reset All Stacks"
                Layout.preferredWidth: 300
                enabled: stacks.length > 0
                onClicked: {
                    MoveCenterTabController.resetAllOffsetStacks()
                    offsetStacksPage.refresh()
                }
            }
            Item { Layout.fillWidth: true }
            MyText {
                text: stacks.length + " / 32 stacks"
                opacity: 0.7
            }
        }
    }
}
