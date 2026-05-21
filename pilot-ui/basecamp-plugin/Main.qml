import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property string result: ""
    property string currentTab: "dashboard"
    property var chatMessages: []

    function call(method, args) {
        if (typeof logos !== "undefined" && logos.callModule)
            return logos.callModule("pilot", method, args)
        return "Bridge unavailable"
    }

    Rectangle {
        anchors.fill: parent
        color: "#0a0a0f"

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Rectangle {
                Layout.fillWidth: true; height: 56; color: "#111127"
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 20; anchors.rightMargin: 20; spacing: 12
                    Text { text: "Pilot Agent"; font.pixelSize: 20; font.bold: true; color: "#ffffff" }
                    Rectangle { width: 8; height: 8; radius: 4; color: "#22c55e" }
                    Text { text: "v1.0.0"; font.pixelSize: 11; color: "#6b7280" }
                    Item { Layout.fillWidth: true }
                    Text { text: "by Johnnie Dom"; font.pixelSize: 11; color: "#6b7280" }
                }
            }

            Rectangle {
                Layout.fillWidth: true; height: 40; color: "#0f0f1e"
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 20; spacing: 0
                    Repeater {
                        model: [
                            {key: "dashboard", label: "Dashboard"},
                            {key: "chat", label: "Chat"},
                            {key: "wallet", label: "Wallet"},
                            {key: "skills", label: "Skills"}
                        ]
                        Rectangle {
                            width: 100; height: 40
                            color: root.currentTab === modelData.key ? "#1a1a2e" : "transparent"
                            radius: 6
                            Text {
                                anchors.centerIn: parent; text: modelData.label; font.pixelSize: 13
                                color: root.currentTab === modelData.key ? "#f59e0b" : "#9ca3af"
                                font.bold: root.currentTab === modelData.key
                            }
                            MouseArea { anchors.fill: parent; onClicked: root.currentTab = modelData.key }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            StackLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                currentIndex: root.currentTab === "dashboard" ? 0 : root.currentTab === "chat" ? 1 : root.currentTab === "wallet" ? 2 : 3

                Item {
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 20; spacing: 16
                        GridLayout {
                            Layout.fillWidth: true; columns: 3; rowSpacing: 12; columnSpacing: 12
                            Repeater {
                                model: [
                                    {title: "Wallet", count: "3", desc: "balance, send, history", clr: "#f59e0b"},
                                    {title: "Storage", count: "4", desc: "upload, download, list, share", clr: "#3b82f6"},
                                    {title: "Messaging", count: "3", desc: "send, join, create group", clr: "#8b5cf6"},
                                    {title: "Agent A2A", count: "5", desc: "card, discover, task, subscribe, cancel", clr: "#10b981"},
                                    {title: "Blockchain", count: "3", desc: "query, call, deploy", clr: "#ef4444"},
                                    {title: "Meta", count: "3", desc: "skills, status, configure", clr: "#6b7280"}
                                ]
                                Rectangle {
                                    Layout.fillWidth: true; height: 80; radius: 10; color: "#1a1a2e"
                                    border.color: modelData.clr; border.width: 1
                                    ColumnLayout {
                                        anchors.fill: parent; anchors.margins: 12; spacing: 4
                                        RowLayout {
                                            Text { text: modelData.title; font.pixelSize: 13; font.bold: true; color: modelData.clr }
                                            Item { Layout.fillWidth: true }
                                            Text { text: modelData.count + " skills"; font.pixelSize: 10; color: "#6b7280" }
                                        }
                                        Text { text: modelData.desc; font.pixelSize: 11; color: "#9ca3af"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                    }
                                }
                            }
                        }
                        RowLayout {
                            spacing: 12
                            Button {
                                text: "Echo Test"
                                onClicked: root.result = root.call("echo", ["hello from basecamp"])
                                contentItem: Text { text: parent.text; color: "#fff"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                                background: Rectangle { implicitWidth: 120; implicitHeight: 36; color: "#238636"; radius: 8 }
                            }
                            Button {
                                text: "Get Status"
                                onClicked: root.result = root.call("metaStatus", [])
                                contentItem: Text { text: parent.text; color: "#fff"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                                background: Rectangle { implicitWidth: 120; implicitHeight: 36; color: "#0969da"; radius: 8 }
                            }
                            Button {
                                text: "List Skills"
                                onClicked: root.result = root.call("metaSkills", [])
                                contentItem: Text { text: parent.text; color: "#fff"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                                background: Rectangle { implicitWidth: 120; implicitHeight: 36; color: "#8b5cf6"; radius: 8 }
                            }
                            Button {
                                text: "Initialize"
                                onClicked: root.result = root.call("initialize", ["/tmp/pilot-data"])
                                contentItem: Text { text: parent.text; color: "#fff"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                                background: Rectangle { implicitWidth: 120; implicitHeight: 36; color: "#f59e0b"; radius: 8 }
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true; height: 100; radius: 10; color: "#1a1a2e"
                            Text { anchors.fill: parent; anchors.margins: 12; text: root.result || "Click a button to test"; font.pixelSize: 12; font.family: "monospace"; color: "#22c55e"; wrapMode: Text.WrapAnywhere }
                        }
                        Rectangle {
                            Layout.fillWidth: true; height: 40; radius: 10; color: "#1a1a2e"
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 12
                                Text { text: "21 Skills"; font.pixelSize: 13; font.bold: true; color: "#fff" }
                                Text { text: "9-State FSM"; font.pixelSize: 11; color: "#9ca3af" }
                                Text { text: "A2A Compatible"; font.pixelSize: 11; color: "#9ca3af" }
                                Item { Layout.fillWidth: true }
                                Text { text: "LP-0008"; font.pixelSize: 12; font.bold: true; color: "#f59e0b" }
                            }
                        }
                        Item { Layout.fillHeight: true }
                    }
                }

                Item {
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 20; spacing: 12
                        Text { text: "Owner Chat"; font.pixelSize: 18; font.bold: true; color: "#fff" }
                        Text { text: "Commands: /status /skills /balance /echo /init /card /files /pending"; font.pixelSize: 11; color: "#6b7280" }
                        Rectangle {
                            Layout.fillWidth: true; Layout.fillHeight: true; radius: 10; color: "#1a1a2e"
                            ListView {
                                id: chatList; anchors.fill: parent; anchors.margins: 12
                                model: root.chatMessages; spacing: 8; clip: true
                                delegate: Rectangle {
                                    width: chatList.width - 24; height: msgT.implicitHeight + 16; radius: 8
                                    color: modelData.from === "you" ? "#1e3a5f" : "#2d1f4e"
                                    Text { id: msgT; anchors.fill: parent; anchors.margins: 8; text: (modelData.from === "you" ? "You: " : "Agent: ") + modelData.text; font.pixelSize: 12; color: "#e5e7eb"; wrapMode: Text.WordWrap }
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true; spacing: 8
                            TextField {
                                id: chatField; Layout.fillWidth: true; placeholderText: "Type a command..."
                                color: "#fff"; placeholderTextColor: "#6b7280"
                                background: Rectangle { radius: 8; color: "#1a1a2e"; border.color: "#374151" }
                                onAccepted: sendBtn.clicked()
                            }
                            Button {
                                id: sendBtn; text: "Send"
                                onClicked: {
                                    var msg = chatField.text.trim()
                                    if (msg.length === 0) return
                                    var msgs = root.chatMessages.slice()
                                    msgs.push({from: "you", text: msg})
                                    var r = ""
                                    if (msg === "/status") r = root.call("metaStatus", [])
                                    else if (msg === "/skills") r = root.call("metaSkills", [])
                                    else if (msg === "/balance") r = root.call("walletBalance", [])
                                    else if (msg === "/init") r = root.call("initialize", ["/tmp/pilot-data"])
                                    else if (msg === "/card") r = root.call("agentCard", [])
                                    else if (msg === "/files") r = root.call("storageList", [])
                                    else if (msg === "/pending") r = root.call("getPendingSpends", [])
                                    else if (msg.startsWith("/echo ")) r = root.call("echo", [msg.substring(6)])
                                    else r = root.call("echo", [msg])
                                    msgs.push({from: "agent", text: r || "No response"})
                                    root.chatMessages = msgs
                                    chatField.text = ""
                                }
                                contentItem: Text { text: parent.text; color: "#fff"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                                background: Rectangle { implicitWidth: 80; implicitHeight: 36; color: "#f59e0b"; radius: 8 }
                            }
                        }
                    }
                }

                Item {
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 20; spacing: 16
                        Text { text: "Wallet"; font.pixelSize: 18; font.bold: true; color: "#fff" }
                        RowLayout {
                            spacing: 12
                            Button {
                                text: "Balance"
                                onClicked: root.result = root.call("walletBalance", [])
                                contentItem: Text { text: parent.text; color: "#fff"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                                background: Rectangle { implicitWidth: 120; implicitHeight: 36; color: "#f59e0b"; radius: 8 }
                            }
                            Button {
                                text: "History"
                                onClicked: root.result = root.call("walletHistory", [])
                                contentItem: Text { text: parent.text; color: "#fff"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                                background: Rectangle { implicitWidth: 120; implicitHeight: 36; color: "#0969da"; radius: 8 }
                            }
                            Button {
                                text: "Pending"
                                onClicked: root.result = root.call("getPendingSpends", [])
                                contentItem: Text { text: parent.text; color: "#fff"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                                background: Rectangle { implicitWidth: 120; implicitHeight: 36; color: "#ef4444"; radius: 8 }
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true; Layout.fillHeight: true; radius: 10; color: "#1a1a2e"
                            Text { anchors.fill: parent; anchors.margins: 12; text: root.result || "Click a button to query wallet"; font.pixelSize: 13; font.family: "monospace"; color: "#22c55e"; wrapMode: Text.WrapAnywhere }
                        }
                    }
                }

                Item {
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 20; spacing: 16
                        Text { text: "All Skills"; font.pixelSize: 18; font.bold: true; color: "#fff" }
                        Button {
                            text: "Load Skills"
                            onClicked: root.result = root.call("metaSkills", [])
                            contentItem: Text { text: parent.text; color: "#fff"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                            background: Rectangle { implicitWidth: 140; implicitHeight: 36; color: "#8b5cf6"; radius: 8 }
                        }
                        Rectangle {
                            Layout.fillWidth: true; Layout.fillHeight: true; radius: 10; color: "#1a1a2e"
                            Text { anchors.fill: parent; anchors.margins: 12; text: root.result || "Click Load Skills"; font.pixelSize: 13; font.family: "monospace"; color: "#22c55e"; wrapMode: Text.WrapAnywhere }
                        }
                    }
                }
            }
        }
    }
}
