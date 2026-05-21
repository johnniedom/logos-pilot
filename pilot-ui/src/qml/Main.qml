import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: root

    readonly property var backend: logos.module("pilot_ui")
    readonly property bool ready: backend !== null && logos.isViewModuleReady("pilot_ui")

    readonly property string balance: backend ? backend.balance : "—"
    readonly property int pendingCount: backend ? backend.pendingTxCount : 0
    readonly property string agentNpk: backend ? backend.agentNpk : ""
    readonly property bool ownerActive: backend ? backend.ownerChannelActive : false

    Rectangle {
        anchors.fill: parent
        color: Theme.palette.gray950
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 0
        spacing: 0

        // Top bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            color: Theme.palette.gray900

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 12

                LogosText {
                    text: "Pilot Agent"
                    font.pixelSize: Theme.typography.h3
                    color: Theme.palette.white
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    width: 8; height: 8; radius: 4
                    color: root.ownerActive ? Theme.palette.green500 : Theme.palette.red500
                }

                LogosText {
                    text: root.ownerActive ? "Connected" : "Offline"
                    font.pixelSize: Theme.typography.caption
                    color: Theme.palette.gray400
                }
            }
        }

        // Tab navigation
        LogosTabBar {
            id: tabBar
            Layout.fillWidth: true

            LogosTabButton { text: "Chat" }
            LogosTabButton { text: "Approvals" }
            LogosTabButton { text: "Status" }
            LogosTabButton { text: "Files" }
        }

        // Content area
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabBar.currentIndex

            // Tab 0: Owner Chat
            ChatPanel {
                backend: root.backend
                ready: root.ready
            }

            // Tab 1: Pending Approvals
            ApprovalsPanel {
                backend: root.backend
                ready: root.ready
            }

            // Tab 2: Agent Status
            StatusPanel {
                balance: root.balance
                agentNpk: root.agentNpk
                ownerActive: root.ownerActive
                pendingCount: root.pendingCount
                backend: root.backend
                ready: root.ready
            }

            // Tab 3: File Browser
            FilesPanel {
                backend: root.backend
                ready: root.ready
            }
        }
    }
}
