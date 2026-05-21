import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: statusPanel

    property string balance: "—"
    property string agentNpk: ""
    property bool ownerActive: false
    property int pendingCount: 0
    property var backend
    property bool ready: false

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        LogosText {
            text: "Agent Status"
            font.pixelSize: Theme.typography.h3
            color: Theme.palette.white
        }

        // Stats grid
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 12
            rowSpacing: 12

            // Balance card
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 80
                radius: 8
                color: Theme.palette.gray875

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 4

                    LogosText {
                        text: "Balance"
                        font.pixelSize: Theme.typography.caption
                        color: Theme.palette.gray400
                    }
                    LogosText {
                        text: statusPanel.balance + " LEZ"
                        font.pixelSize: Theme.typography.h2
                        color: Theme.palette.white
                    }
                }
            }

            // Pending card
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 80
                radius: 8
                color: Theme.palette.gray875

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 4

                    LogosText {
                        text: "Pending"
                        font.pixelSize: Theme.typography.caption
                        color: Theme.palette.gray400
                    }
                    LogosText {
                        text: statusPanel.pendingCount.toString()
                        font.pixelSize: Theme.typography.h2
                        color: statusPanel.pendingCount > 0
                            ? Theme.palette.orange400
                            : Theme.palette.white
                    }
                }
            }

            // Owner channel card
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 80
                radius: 8
                color: Theme.palette.gray875

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 4

                    LogosText {
                        text: "Owner Channel"
                        font.pixelSize: Theme.typography.caption
                        color: Theme.palette.gray400
                    }
                    RowLayout {
                        spacing: 6
                        Rectangle {
                            width: 8; height: 8; radius: 4
                            color: statusPanel.ownerActive
                                ? Theme.palette.green500
                                : Theme.palette.red500
                        }
                        LogosText {
                            text: statusPanel.ownerActive ? "Active" : "Disconnected"
                            font.pixelSize: Theme.typography.body
                            color: Theme.palette.white
                        }
                    }
                }
            }

            // Network card
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 80
                radius: 8
                color: Theme.palette.gray875

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 4

                    LogosText {
                        text: "Network"
                        font.pixelSize: Theme.typography.caption
                        color: Theme.palette.gray400
                    }
                    LogosText {
                        text: "LEZ Testnet"
                        font.pixelSize: Theme.typography.body
                        color: Theme.palette.white
                    }
                }
            }
        }

        // Agent NPK
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            radius: 8
            color: Theme.palette.gray875

            RowLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                ColumnLayout {
                    spacing: 2
                    LogosText {
                        text: "Agent NPK"
                        font.pixelSize: Theme.typography.caption
                        color: Theme.palette.gray400
                    }
                    LogosText {
                        text: statusPanel.agentNpk || "Not initialized"
                        font.pixelSize: Theme.typography.mono
                        color: Theme.palette.gray200
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
