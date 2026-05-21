import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: filesPanel

    property var backend
    property bool ready: false

    property var fileList: []

    Component.onCompleted: refreshFiles()

    function refreshFiles() {
        if (!ready) return
        logos.watch(backend.getFiles(),
            function(result) {
                try {
                    var data = JSON.parse(result)
                    filesPanel.fileList = data.files || []
                } catch(e) {
                    filesPanel.fileList = []
                }
            },
            function(error) {}
        )
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true

            LogosText {
                text: "Encrypted Storage"
                font.pixelSize: Theme.typography.h3
                color: Theme.palette.white
            }

            Item { Layout.fillWidth: true }

            LogosText {
                text: filesPanel.fileList.length + " files"
                font.pixelSize: Theme.typography.caption
                color: Theme.palette.gray400
            }
        }

        LogosScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: filesList
                model: filesPanel.fileList
                spacing: 8
                clip: true

                delegate: Rectangle {
                    width: filesList.width - 16
                    height: 64
                    radius: 8
                    color: Theme.palette.gray875

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 12

                        Rectangle {
                            width: 40; height: 40; radius: 8
                            color: Theme.palette.gray800

                            LogosText {
                                anchors.centerIn: parent
                                text: "📄"
                                font.pixelSize: 18
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            LogosText {
                                text: modelData.label
                                font.pixelSize: Theme.typography.body
                                color: Theme.palette.white
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            LogosText {
                                text: modelData.cid
                                font.pixelSize: Theme.typography.caption
                                color: Theme.palette.gray500
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }

                        LogosText {
                            text: modelData.timestamp
                            font.pixelSize: Theme.typography.caption
                            color: Theme.palette.gray500
                        }
                    }
                }
            }
        }

        // Empty state
        LogosText {
            visible: filesPanel.fileList.length === 0
            text: "No files stored yet"
            font.pixelSize: Theme.typography.body
            color: Theme.palette.gray500
            Layout.alignment: Qt.AlignHCenter
        }
    }
}
