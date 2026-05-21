import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: approvalsPanel

    property var backend
    property bool ready: false

    property var pendingList: []

    Component.onCompleted: refreshPending()

    function refreshPending() {
        if (!ready) return
        logos.watch(backend.getPending(),
            function(result) {
                try {
                    var data = JSON.parse(result)
                    approvalsPanel.pendingList = data.pending || []
                } catch(e) {
                    approvalsPanel.pendingList = []
                }
            },
            function(error) {}
        )
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        LogosText {
            text: "Pending Approvals"
            font.pixelSize: Theme.typography.h3
            color: Theme.palette.white
        }

        LogosScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: approvalsList
                model: approvalsPanel.pendingList
                spacing: 12
                clip: true

                delegate: Rectangle {
                    width: approvalsList.width - 16
                    height: approvalContent.implicitHeight + 24
                    radius: 8
                    color: Theme.palette.gray875
                    border.color: Theme.palette.orange400Opacity30
                    border.width: 1

                    ColumnLayout {
                        id: approvalContent
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true

                            LogosText {
                                text: modelData.amount + " LEZ"
                                font.pixelSize: Theme.typography.h4
                                color: Theme.palette.orange400
                            }

                            Item { Layout.fillWidth: true }

                            LogosBadge {
                                text: modelData.state
                            }
                        }

                        LogosText {
                            text: "To: " + modelData.recipient
                            font.pixelSize: Theme.typography.body
                            color: Theme.palette.gray400
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }

                        LogosText {
                            text: "Reason: " + modelData.reason
                            font.pixelSize: Theme.typography.caption
                            color: Theme.palette.gray500
                            Layout.fillWidth: true
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            LogosButton {
                                text: "Approve"
                                onClicked: {
                                    logos.watch(backend.approve(modelData.id),
                                        function() { approvalsPanel.refreshPending() },
                                        function() {}
                                    )
                                }
                            }

                            LogosButton {
                                text: "Reject"
                                onClicked: {
                                    logos.watch(backend.reject(modelData.id),
                                        function() { approvalsPanel.refreshPending() },
                                        function() {}
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }

        // Empty state
        LogosText {
            visible: approvalsPanel.pendingList.length === 0
            text: "No pending approvals"
            font.pixelSize: Theme.typography.body
            color: Theme.palette.gray500
            Layout.alignment: Qt.AlignHCenter
        }
    }
}
