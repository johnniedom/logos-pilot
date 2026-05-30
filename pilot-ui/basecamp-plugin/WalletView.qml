import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: walletView

    property string balance: "—"
    property string account: ""
    property var history: []
    property var pending: []
    property string sendMsg: ""

    function call(method, args) {
        if (typeof logos === "undefined" || !logos.callModule)
            return JSON.stringify({error: "Not connected"})
        try { return logos.callModule("pilot", method, args || []) }
        catch (e) { return JSON.stringify({error: e.message}) }
    }

    function parseResult(raw) {
        try {
            var obj = JSON.parse(raw)
            if (typeof obj === "string") obj = JSON.parse(obj)
            return obj
        } catch (e) { return {error: raw} }
    }

    function refresh() {
        try {
            var br = parseResult(call("walletBalance"))
            if (!br.error) { balance = br.balance || "0"; account = br.account || "" }
        } catch (e) {}
        try {
            var hr = parseResult(call("walletHistory"))
            history = hr.history || []
        } catch (e) {}
        try {
            var pr = parseResult(call("getPendingSpends"))
            pending = pr.pending || []
        } catch (e) {}
    }

    function sendTokens() {
        var to = sendTo.text.trim()
        var amt = sendAmt.text.trim()
        var reason = sendReason.text.trim()
        if (!to || !amt) { sendMsg = "Enter recipient and amount"; return }
        sendMsg = ""
        var raw = call("walletSend", [to, amt, reason])
        try {
            var r = parseResult(raw)
            if (r.error) { sendMsg = "Error: " + r.error; return }
            if (r.tx_hash) sendMsg = "Sent " + (r.amount || amt) + " LEZ"
            else if (r.request_id) sendMsg = "Pending approval: #" + r.request_id
            else sendMsg = raw
            sendTo.text = ""; sendAmt.text = ""; sendReason.text = ""
            refresh()
        } catch (e) { sendMsg = raw }
    }

    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: content
            width: flick.width
            spacing: Theme.spacing.large

            Item { Layout.preferredHeight: Theme.spacing.small }

            // ── Pending approvals (top priority) ──
            Rectangle {
                visible: walletView.pending.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                Layout.preferredHeight: pendTopCol.implicitHeight + Theme.spacing.large * 2
                radius: Theme.spacing.radiusLarge
                color: Theme.palette.backgroundTertiary
                border.color: Theme.palette.warning
                border.width: 1

                ColumnLayout {
                    id: pendTopCol
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.large
                    spacing: Theme.spacing.small

                    LogosText {
                        text: walletView.pending.length + " Pending Approval" + (walletView.pending.length > 1 ? "s" : "")
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.warning
                    }

                    Repeater {
                        model: walletView.pending

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.medium

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                LogosText {
                                    text: (modelData.amount || 0) + " LEZ → " + (modelData.recipient || "?")
                                    font.pixelSize: Theme.typography.primaryText
                                    font.weight: Theme.typography.weightMedium
                                    color: Theme.palette.text
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                }
                                LogosText {
                                    visible: (modelData.reason || "") !== ""
                                    text: modelData.reason || ""
                                    font.pixelSize: Theme.typography.secondaryText
                                    color: Theme.palette.textTertiary
                                }
                            }

                            LogosButton {
                                text: "Approve"
                                onClicked: {
                                    call("approveSpend", [modelData.id])
                                    walletView.refresh()
                                }
                            }
                            LogosButton {
                                text: "Reject"
                                onClicked: {
                                    call("rejectSpend", [modelData.id])
                                    walletView.refresh()
                                }
                            }
                        }
                    }
                }
            }

            // ── Agent Wallet ──
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                spacing: Theme.spacing.medium

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 100
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.large
                        spacing: Theme.spacing.tiny

                        LogosText {
                            text: "Agent Wallet"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        LogosText {
                            text: walletView.balance + " LEZ"
                            font.pixelSize: Theme.typography.titleText
                            font.weight: Theme.typography.weightBold
                            color: Theme.palette.text
                        }
                        LogosText {
                            text: "Limit: 100 LEZ/tx · 500 LEZ/24h"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textTertiary
                        }
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 140
                    Layout.preferredHeight: 100
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.large
                        spacing: Theme.spacing.tiny

                        LogosText {
                            text: "Pending"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        LogosText {
                            text: walletView.pending.length.toString()
                            font.pixelSize: Theme.typography.titleText
                            font.weight: Theme.typography.weightBold
                            color: walletView.pending.length > 0
                                ? Theme.palette.warning : Theme.palette.text
                        }
                    }
                }
            }

            // ── Fund Agent ──
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                Layout.preferredHeight: fundCol.implicitHeight + Theme.spacing.medium * 2
                radius: Theme.spacing.radiusLarge
                color: Theme.palette.backgroundTertiary

                ColumnLayout {
                    id: fundCol
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.medium
                    spacing: Theme.spacing.tiny

                    LogosText {
                        text: "Fund this agent"
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textSecondary
                    }
                    LogosText {
                        text: walletView.account
                        font.pixelSize: Theme.typography.primaryText
                        color: Theme.palette.text
                        Layout.fillWidth: true
                    }
                    LogosText {
                        text: "Send LEZ to this address from your wallet"
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textTertiary
                    }
                }
            }

            // ── Send ──
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                Layout.preferredHeight: sendCol.implicitHeight + Theme.spacing.large * 2
                radius: Theme.spacing.radiusLarge
                color: Theme.palette.backgroundTertiary

                ColumnLayout {
                    id: sendCol
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.large
                    spacing: Theme.spacing.small

                    LogosText {
                        text: "Send"
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.text
                    }

                    LogosTextField {
                        id: sendTo
                        Layout.fillWidth: true
                        placeholderText: "Recipient address"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing.small
                        LogosTextField {
                            id: sendAmt
                            Layout.preferredWidth: 140
                            placeholderText: "Amount"
                        }
                        LogosText {
                            text: "LEZ"
                            font.pixelSize: Theme.typography.primaryText
                            color: Theme.palette.textSecondary
                        }
                        Item { Layout.fillWidth: true }
                        LogosTextField {
                            id: sendReason
                            Layout.fillWidth: true
                            placeholderText: "Reason (optional)"
                        }
                    }

                    RowLayout {
                        spacing: Theme.spacing.medium
                        LogosButton {
                            text: "Send"
                            onClicked: walletView.sendTokens()
                        }
                        LogosText {
                            visible: walletView.sendMsg !== ""
                            text: walletView.sendMsg
                            font.pixelSize: Theme.typography.secondaryText
                            color: walletView.sendMsg.indexOf("Error") >= 0
                                ? Theme.palette.error : Theme.palette.success
                        }
                    }
                }
            }

            // ── History ──
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                spacing: Theme.spacing.small

                RowLayout {
                    Layout.fillWidth: true
                    LogosText {
                        text: "History"
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.text
                    }
                    Item { Layout.fillWidth: true }
                    LogosButton {
                        text: "Refresh"
                        onClicked: walletView.refresh()
                    }
                }

                LogosText {
                    visible: walletView.history.length === 0
                    text: "No transactions yet"
                    font.pixelSize: Theme.typography.primaryText
                    color: Theme.palette.textTertiary
                }

                Repeater {
                    model: walletView.history

                    Rectangle {
                        Layout.fillWidth: true
                        height: 48
                        radius: Theme.spacing.radiusMedium
                        color: Theme.palette.backgroundTertiary

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spacing.medium
                            anchors.rightMargin: Theme.spacing.medium
                            spacing: Theme.spacing.medium

                            LogosText {
                                text: modelData.type || "?"
                                font.pixelSize: Theme.typography.primaryText
                                font.weight: Theme.typography.weightMedium
                                color: Theme.palette.text
                                Layout.preferredWidth: 80
                            }
                            LogosText {
                                text: (modelData.amount || 0) + " LEZ"
                                font.pixelSize: Theme.typography.primaryText
                                color: Theme.palette.text
                            }
                            Item { Layout.fillWidth: true }
                            LogosText {
                                text: modelData.timestamp || ""
                                font.pixelSize: Theme.typography.secondaryText
                                color: Theme.palette.textTertiary
                            }
                        }
                    }
                }
            }

            Item { Layout.preferredHeight: Theme.spacing.large }
        }
    }
}
