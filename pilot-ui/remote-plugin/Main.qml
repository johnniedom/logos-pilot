// Pilot Remote — talk to a Pilot agent that runs on ANOTHER machine, from this Basecamp, over
// Logos Messaging. Nothing here touches a key or the network: every such step is a call into the
// pilot_owner module (pilot-owner/module), which signs and seals the owner's line, publishes it
// through a Waku relay's REST API, and opens the agent's sealed replies read back from the relay
// store. No local agent, no daemon socket, no server of ours in between.
//
// Two screens. Set-up: make (or import) the owner key, bind the agent to it when deploying it,
// paste the agent's card + account id + relay. Chat: type a line, replies arrive every 5 s.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: root

    property var status: ({})
    readonly property bool hasKey: status.initialised === true
    readonly property bool paired: status.paired === true
    property bool showSetup: true
    property string lastError: ""

    function call(method, args) {
        if (typeof logos === "undefined" || !logos.callModule)
            return JSON.stringify({ok: false, error: "Not running in Basecamp"})
        try { return logos.callModule("pilot_owner", method, args || []) }
        catch (e) { return JSON.stringify({ok: false, error: e.message}) }
    }

    function parse(raw) {
        try {
            var o = JSON.parse(raw)
            if (typeof o === "string") o = JSON.parse(o)
            return o
        } catch (e) { return {ok: false, error: String(raw)} }
    }

    function refreshStatus() {
        var s = parse(call("status"))
        if (s && s.ok === false) { lastError = s.error || "pilot_owner module not reachable"; return }
        status = s || {}
        showSetup = !(status.paired === true)
    }

    ListModel { id: messages }

    function addMsg(from, text) {
        messages.append({from: from, text: text})
        if (messages.count > 200) messages.remove(0)
        Qt.callLater(function() { msgList.positionViewAtEnd() })
    }

    function sendLine(text) {
        text = text.trim()
        if (text.length === 0) return
        var r = parse(call("send", [text]))
        if (r.ok) { addMsg("you", text); lastError = "" }
        else { lastError = r.error || "send failed"; addMsg("system", "Not sent: " + lastError) }
    }

    function pollReplies() {
        if (!paired) return
        var r = parse(call("poll"))
        if (!r.ok) { lastError = r.error || "poll failed"; return }
        lastError = ""
        var list = r.replies || []
        for (var i = 0; i < list.length; i++) addMsg("agent", list[i].text)
    }

    Component.onCompleted: refreshStatus()

    Timer {
        interval: 5000
        repeat: true
        running: root.paired && !root.showSetup
        onTriggered: root.pollReplies()
    }

    Rectangle { anchors.fill: parent; color: Theme.palette.backgroundElevated }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            color: Theme.palette.background

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacing.large
                anchors.rightMargin: Theme.spacing.large
                spacing: Theme.spacing.medium

                LogosText {
                    text: "Pilot Remote"
                    font.pixelSize: Theme.typography.panelTitleText
                    font.weight: Theme.typography.weightBold
                    color: Theme.palette.text
                }
                Rectangle {
                    width: 8; height: 8; radius: Theme.spacing.radiusSmall
                    color: root.paired ? Theme.palette.success : Theme.palette.error
                }
                LogosText {
                    text: root.paired ? ("Paired with " + (root.status.agent_name || "agent")) : "Not paired"
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.textSecondary
                }
                Item { Layout.fillWidth: true }
                LogosText {
                    visible: root.paired
                    text: "relay " + (root.status.relay || "")
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.textTertiary
                }
                LogosButton {
                    visible: root.paired
                    text: root.showSetup ? "Back to chat" : "Change agent"
                    onClicked: root.showSetup = !root.showSetup
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.palette.borderSecondary }

        // ═══ Set-up ═══
        Flickable {
            visible: root.showSetup
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: setupCol.implicitHeight + 2 * Theme.spacing.large
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: setupCol
                x: Theme.spacing.large
                y: Theme.spacing.large
                width: parent.width - 2 * Theme.spacing.large
                spacing: Theme.spacing.medium

                LogosText {
                    text: "1. Your key"
                    font.pixelSize: Theme.typography.primaryText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }
                LogosText {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.hasKey
                        ? "Your owner key is ready. The agent must be bound to it: deploy the agent with the line below, or run it on an agent that is already up."
                        : "Make the key the agent will trust. Every line you send is signed with it and every reply is sealed to it. It stays in " + (root.status.state || "~/.pilot-owner/state.json") + " on this machine."
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.textSecondary
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.medium
                    LogosButton {
                        text: root.hasKey ? "Key ready" : "Create my key"
                        enabled: !root.hasKey
                        onClicked: {
                            var r = root.parse(root.call("createKey"))
                            if (r.ok) { root.lastError = ""; root.refreshStatus(); root.showSetup = true }
                            else root.lastError = r.error || "createKey failed"
                        }
                    }
                    LogosTextField {
                        id: importField
                        Layout.fillWidth: true
                        placeholderText: "or paste <private hex>:<public hex> from `pilot-owner init` on another device"
                    }
                    LogosButton {
                        text: "Import"
                        enabled: importField.text.trim().length > 0
                        onClicked: {
                            var r = root.parse(root.call("importKey", [importField.text.trim()]))
                            if (r.ok) { root.lastError = ""; importField.text = ""; root.refreshStatus(); root.showSetup = true }
                            else root.lastError = r.error || "import failed"
                        }
                    }
                }
                TextField {
                    visible: root.hasKey
                    Layout.fillWidth: true
                    readOnly: true
                    selectByMouse: true
                    text: "PILOT_OWNER_NPK=" + (root.status.owner_pub || "") + " pilot deploy"
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.text
                    background: Rectangle {
                        color: Theme.palette.backgroundTertiary
                        radius: Theme.spacing.radiusSmall
                        border.color: Theme.palette.borderSecondary
                    }
                }
                LogosText {
                    visible: root.hasKey
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "Already running? On the agent's machine:  logoscore call pilot metaConfigure owner.npk " + (root.status.owner_pub || "")
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.textTertiary
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.palette.borderSecondary }

                LogosText {
                    text: "2. Your agent"
                    font.pixelSize: Theme.typography.primaryText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }
                LogosText {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "Paste the agent's card (`pilot card`, or `logoscore call pilot agentCard`), its account id (`pilot status`, or metaStatus.account), and the REST address of the relay both sides use."
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.textSecondary
                }
                TextArea {
                    id: cardField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 110
                    placeholderText: "{ \"name\": \"Pilot-…\", \"_logos\": { \"signing_key\": \"04…\", … } }"
                    wrapMode: TextEdit.Wrap
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.text
                    background: Rectangle {
                        color: Theme.palette.backgroundTertiary
                        radius: Theme.spacing.radiusSmall
                        border.color: Theme.palette.borderSecondary
                    }
                }
                LogosTextField {
                    id: accountField
                    Layout.fillWidth: true
                    placeholderText: "agent account id (64 hex characters)"
                }
                LogosTextField {
                    id: relayField
                    Layout.fillWidth: true
                    placeholderText: "relay REST URL, e.g. http://127.0.0.1:8645"
                    text: root.status.relay || ""
                }
                LogosButton {
                    text: "Pair"
                    enabled: root.hasKey && cardField.text.trim().length > 0 && accountField.text.trim().length > 0
                    onClicked: {
                        var r = root.parse(root.call("pair", [cardField.text, accountField.text.trim(), relayField.text.trim()]))
                        if (r.ok) {
                            root.lastError = ""
                            root.refreshStatus()
                            root.showSetup = false
                            root.addMsg("system", "Paired with " + (r.agent_name || "agent") + " on " + r.topic + " via " + r.relay)
                        } else root.lastError = r.error || "pair failed"
                    }
                }
                LogosText {
                    visible: root.lastError.length > 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.lastError
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.error
                }
            }
        }

        // ═══ Chat ═══
        ColumnLayout {
            visible: !root.showSetup
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            ListView {
                id: msgList
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Theme.spacing.large
                model: messages
                clip: true
                spacing: Theme.spacing.medium
                delegate: Rectangle {
                    width: msgList.width
                    height: bubble.implicitHeight + Theme.spacing.medium * 2
                    radius: Theme.spacing.radiusSmall
                    color: model.from === "you" ? Theme.palette.background
                         : model.from === "agent" ? Theme.palette.backgroundTertiary
                         : "transparent"
                    ColumnLayout {
                        id: bubble
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.spacing.medium
                        spacing: 2
                        LogosText {
                            text: model.from === "you" ? "you" : model.from === "agent" ? (root.status.agent_name || "agent") : "note"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textTertiary
                        }
                        LogosText {
                            Layout.fillWidth: true
                            text: model.text
                            wrapMode: Text.WrapAnywhere
                            font.pixelSize: Theme.typography.primaryText
                            color: Theme.palette.text
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.palette.borderSecondary }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.spacing.medium
                spacing: Theme.spacing.medium
                LogosButton { text: "/balance"; onClicked: root.sendLine("/balance") }
                LogosButton { text: "/pending"; onClicked: root.sendLine("/pending") }
                LogosButton { text: "/status";  onClicked: root.sendLine("/status") }
                LogosButton { text: "/help";    onClicked: root.sendLine("/help") }
                Item { Layout.fillWidth: true }
                LogosText {
                    text: root.lastError.length > 0 ? root.lastError : "replies every 5 s"
                    font.pixelSize: Theme.typography.secondaryText
                    color: root.lastError.length > 0 ? Theme.palette.error : Theme.palette.textTertiary
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.medium
                Layout.rightMargin: Theme.spacing.medium
                Layout.bottomMargin: Theme.spacing.medium
                spacing: Theme.spacing.medium
                LogosTextField {
                    id: input
                    Layout.fillWidth: true
                    placeholderText: "/send <to> <amount> <reason>    /approve <id>    /reject <id>    or ask in words"
                    onAccepted: { root.sendLine(text); text = "" }
                }
                LogosButton {
                    text: "Send"
                    enabled: input.text.trim().length > 0
                    onClicked: { root.sendLine(input.text); input.text = "" }
                }
            }
        }
    }
}
