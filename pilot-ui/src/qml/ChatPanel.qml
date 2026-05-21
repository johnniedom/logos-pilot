import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: chatPanel

    property var backend
    property bool ready: false

    property var messages: []

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // Message list
        LogosScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: messageList
                model: chatPanel.messages
                spacing: 8
                clip: true

                delegate: Rectangle {
                    width: messageList.width - 32
                    height: msgContent.implicitHeight + 16
                    radius: 8
                    color: modelData.from === "agent"
                        ? Theme.palette.gray875
                        : Theme.palette.gray800

                    anchors.right: modelData.from === "owner" ? parent.right : undefined

                    ColumnLayout {
                        id: msgContent
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 4

                        LogosText {
                            text: modelData.from === "agent" ? "Pilot" : "You"
                            font.pixelSize: Theme.typography.caption
                            color: modelData.from === "agent"
                                ? Theme.palette.orange400
                                : Theme.palette.blue400
                        }

                        LogosText {
                            text: modelData.text
                            font.pixelSize: Theme.typography.body
                            color: Theme.palette.white
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }

        // Input area
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            LogosTextField {
                id: chatInput
                Layout.fillWidth: true
                placeholderText: "/approve, /reject, /status, /balance..."

                Keys.onReturnPressed: sendBtn.clicked()
            }

            LogosButton {
                id: sendBtn
                text: "Send"
                enabled: chatPanel.ready && chatInput.text.length > 0

                onClicked: {
                    if (!chatInput.text) return

                    var cmd = chatInput.text
                    chatPanel.messages = chatPanel.messages.concat([{from: "owner", text: cmd}])

                    logos.watch(backend.sendCommand(cmd),
                        function(response) {
                            chatPanel.messages = chatPanel.messages.concat([{from: "agent", text: response}])
                        },
                        function(error) {
                            chatPanel.messages = chatPanel.messages.concat([{from: "agent", text: "Error: " + error}])
                        }
                    )
                    chatInput.text = ""
                }
            }
        }
    }
}
