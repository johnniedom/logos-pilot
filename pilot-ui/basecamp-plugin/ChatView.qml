import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: chatView
    property bool busy: false

    function call(method, args) {
        if (typeof logos === "undefined" || !logos.callModule)
            return '{"error":"Not connected"}'
        try { return logos.callModule("pilot", method, args || []) }
        catch (e) { return '{"error":"' + e.message + '"}' }
    }

    function parseResult(raw) {
        try {
            var obj = JSON.parse(raw)
            if (typeof obj === "string") obj = JSON.parse(obj)
            return obj
        } catch (e) { return {error: raw} }
    }

    ListModel { id: messages }

    function addMsg(from, text) {
        messages.append({from: from, text: text})
        if (messages.count > 100) messages.remove(0)
        Qt.callLater(function() { msgList.positionViewAtEnd() })
    }

    // ── Action dispatch (mirrors CLI dispatchAction) ──

    function dispatchAction(j) {
        var act = j.action || "none"
        var p = j.params || {}
        switch (act) {
        case "balance": return call("walletBalance")
        case "send":
            return call("walletSend", [p.recipient || "", String(p.amount || 0), p.reason || ""])
        case "upload":
            return call("storageUpload", [p.path || "", p.label || ""])
        case "download":
            var cid = p.cid || p.label || ""
            if (cid && cid.charAt(0) !== "z") {
                try {
                    var fl = parseResult(call("storageList"))
                    for (var i = 0; i < fl.files.length; i++)
                        if (fl.files[i].label === cid) { cid = fl.files[i].cid; break }
                } catch (e) {}
            }
            return call("storageDownload", [cid, p.path || "/tmp/download"])
        case "files": return call("storageList")
        case "history": return call("walletHistory")
        case "skills": return call("metaSkills")
        case "status": return call("metaStatus")
        case "discover": return call("agentDiscover", ["pilot"])
        case "approve": return call("approveSpend", [p.id || ""])
        case "reject": return call("rejectSpend", [p.id || ""])
        case "pending": return call("getPendingSpends")
        default: return p.text || ""
        }
    }

    // ── Slash commands (bypass LLM) ──

    function handleSlash(line) {
        var parts = line.trim().split(/\s+/)
        var cmd = parts[0].toLowerCase()
        switch (cmd) {
        case "/help":
            return JSON.stringify({text: "/balance  /history  /send <to> <amt> <reason>\n/upload <path> <label>  /download <label> <path>  /files\n/skills  /status  /discover  /pending\n/approve <id>  /reject <id>  /quit"})
        case "/balance": return call("walletBalance")
        case "/history": return call("walletHistory")
        case "/skills": return call("metaSkills")
        case "/status": return call("metaStatus")
        case "/files": return call("storageList")
        case "/pending": return call("getPendingSpends")
        case "/discover": return call("agentDiscover", [parts[1] || "pilot"])
        case "/send":
            if (parts.length < 4) return JSON.stringify({error: "/send <recipient> <amount> <reason>"})
            return call("walletSend", [parts[1], parts[2], parts.slice(3).join(" ")])
        case "/upload":
            if (parts.length < 3) return JSON.stringify({error: "/upload <path> <label>"})
            return call("storageUpload", [parts[1], parts[2]])
        case "/download":
            if (parts.length < 3) return JSON.stringify({error: "/download <label-or-cid> <path>"})
            var dlCid = parts[1]
            if (dlCid.charAt(0) !== "z") {
                try {
                    var fl = parseResult(call("storageList"))
                    for (var i = 0; i < fl.files.length; i++)
                        if (fl.files[i].label === dlCid) { dlCid = fl.files[i].cid; break }
                } catch (e) {}
            }
            return call("storageDownload", [dlCid, parts[2]])
        case "/approve":
            return parts.length >= 2 ? call("approveSpend", [parts[1]])
                : JSON.stringify({error: "/approve <id>"})
        case "/reject":
            return parts.length >= 2 ? call("rejectSpend", [parts[1]])
                : JSON.stringify({error: "/reject <id>"})
        default:
            return JSON.stringify({error: "Unknown: " + cmd + " — type /help"})
        }
    }

    // ── Format JSON results for display ──

    function formatResult(raw) {
        try {
            var j = (typeof raw === "object") ? raw : parseResult(raw)
            if (j.error) return "Error: " + j.error
            if (j.text) return j.text

            if (j.skills && j.count) {
                var lines = [j.count + " skills:"]
                var lastCat = ""
                for (var i = 0; i < j.skills.length; i++) {
                    var s = j.skills[i]
                    if (s.category !== lastCat) {
                        lines.push("\n" + s.category.toUpperCase())
                        lastCat = s.category
                    }
                    var price = (s.price_lez || 0) > 0 ? " (" + s.price_lez + " LEZ)" : ""
                    lines.push("  " + s.name + " — " + s.description + price)
                }
                return lines.join("\n")
            }
            if (j.balance !== undefined && j.account)
                return "Balance: " + (j.balance || "0") + " LEZ\nAccount: " + j.account
            if (j.files && Array.isArray(j.files)) {
                if (j.files.length === 0) return "No stored files"
                var fl = [j.files.length + " files:"]
                for (var i = 0; i < j.files.length; i++)
                    fl.push("  " + j.files[i].label + " — " + j.files[i].cid)
                return fl.join("\n")
            }
            if (j.cid && j.label && j.encrypted !== undefined)
                return "Uploaded: " + j.label + "\nCID: " + j.cid
            if (j.path && j.decrypted !== undefined)
                return "Downloaded to: " + j.path
            if (j.initialized !== undefined) {
                var st = "Status: " + (j.initialized ? "online" : "offline")
                st += "\nAccount: " + (j.account || "—")
                if (j.llm && typeof j.llm === "object")
                    st += "\nLLM: " + j.llm.provider + " / " + j.llm.model
                if (j.owner_name) st += "\nOwner: " + j.owner_name
                return st
            }
            if (j.history && Array.isArray(j.history)) {
                if (j.history.length === 0) return "No transactions"
                var hl = ["Transactions:"]
                for (var i = 0; i < j.history.length; i++)
                    hl.push("  " + j.history[i].type + "  " + j.history[i].amount + " LEZ  " + (j.history[i].timestamp || ""))
                return hl.join("\n")
            }
            if (j.pending && Array.isArray(j.pending)) {
                if (j.pending.length === 0) return "No pending requests"
                var pl = ["Pending:"]
                for (var i = 0; i < j.pending.length; i++)
                    pl.push("  #" + j.pending[i].id + ": " + j.pending[i].amount + " LEZ → " + (j.pending[i].recipient || ""))
                return pl.join("\n")
            }
            if (j.tx_hash)
                return "Transfer sent\nTX: " + j.tx_hash + "\nAmount: " + (j.amount || 0) + " LEZ"
            if (j.request_id && j.state)
                return "Spend request #" + j.request_id + " — " + j.state
            if (j.sent && j.recipient)
                return "Message sent to " + j.recipient
            if (j.shared && j.cid)
                return "Shared " + j.cid + " with " + (j.recipient || "")
            if (j.agents && Array.isArray(j.agents)) {
                if (j.agents.length === 0) return "No agents discovered"
                var al = ["Agents found:"]
                for (var i = 0; i < j.agents.length; i++)
                    al.push("  " + (j.agents[i].name || j.agents[i].npk || "agent"))
                return al.join("\n")
            }
            return raw
        } catch (e) { return raw }
    }

    // ── Send message (natural language or slash command) ──

    function send() {
        var text = input.text.trim()
        if (!text) return
        input.text = ""
        addMsg("owner", text)
        busy = true

        if (text.charAt(0) === "/") {
            addMsg("agent", formatResult(handleSlash(text)))
            busy = false
            return
        }

        var raw = call("processOwnerMessage", [text])
        try {
            var j = parseResult(raw)
            var action = j.action || "reply"

            if (action === "reply" || action === "none") {
                addMsg("agent", (j.params && j.params.text) ? j.params.text : (raw || "…"))
            } else {
                var result = dispatchAction(j)
                var formatted = formatResult(result)
                var feedback = call("processOwnerMessage",
                    ["[System: the action you dispatched returned this result] " + formatted])
                try {
                    var fj = parseResult(feedback)
                    addMsg("agent", (fj.params && fj.params.text) ? fj.params.text : formatted)
                } catch (e) {
                    addMsg("agent", formatted)
                }
            }
        } catch (e) {
            addMsg("agent", raw || "No response")
        }
        busy = false
    }

    // ── Layout ──

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: msgList
                anchors.fill: parent
                model: messages
                spacing: Theme.spacing.small
                clip: true
                topMargin: Theme.spacing.large
                bottomMargin: Theme.spacing.large

                onCountChanged: Qt.callLater(positionViewAtEnd)

                delegate: Item {
                    width: msgList.width
                    height: bubble.height + Theme.spacing.medium

                    Rectangle {
                        id: bubble
                        property real maxW: msgList.width * 0.85 - Theme.spacing.large * 2
                        width: model.from === "agent" ? maxW
                            : Math.min(msgText.implicitWidth + Theme.spacing.xlarge * 2, maxW)
                        height: msgCol.implicitHeight + Theme.spacing.large * 2
                        radius: Theme.spacing.radiusXlarge
                        x: model.from === "owner"
                            ? parent.width - width - Theme.spacing.large
                            : Theme.spacing.large
                        color: model.from === "owner"
                            ? Theme.palette.backgroundSecondary
                            : Theme.palette.backgroundTertiary

                        ColumnLayout {
                            id: msgCol
                            anchors.fill: parent
                            anchors.margins: Theme.spacing.large
                            spacing: Theme.spacing.small

                            LogosText {
                                text: model.from === "agent" ? "Pilot" : "You"
                                font.pixelSize: Theme.typography.secondaryText
                                font.weight: Theme.typography.weightBold
                                color: model.from === "agent"
                                    ? Theme.palette.primary : Theme.palette.info
                            }

                            LogosText {
                                id: msgText
                                text: model.text
                                font.pixelSize: Theme.typography.primaryText
                                color: Theme.palette.text
                                wrapMode: Text.Wrap
                                lineHeight: 1.4
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }

            LogosText {
                visible: messages.count === 0
                anchors.centerIn: parent
                text: "Type a message or /help for commands"
                font.pixelSize: Theme.typography.subtitleText
                color: Theme.palette.textTertiary
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.palette.borderSecondary }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            color: Theme.palette.background

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacing.large
                anchors.rightMargin: Theme.spacing.large
                anchors.topMargin: Theme.spacing.medium
                anchors.bottomMargin: Theme.spacing.medium
                spacing: Theme.spacing.small

                LogosTextField {
                    id: input
                    Layout.fillWidth: true
                    placeholderText: chatView.busy ? "Thinking..." : "Message or /command..."
                    enabled: !chatView.busy
                    Keys.onReturnPressed: chatView.send()
                    Keys.onEnterPressed: chatView.send()
                }

                LogosButton {
                    text: chatView.busy ? "..." : "Send"
                    enabled: !chatView.busy && input.text.trim().length > 0
                    onClicked: chatView.send()
                }
            }
        }
    }
}
