import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: root

    property bool agentOnline: false
    property string accountId: ""
    property string ownerName: ""
    property string llmProvider: ""
    property string llmModel: ""
    property bool setupDone: false

    function call(method, args) {
        if (typeof logos === "undefined" || !logos.callModule)
            return JSON.stringify({error: "Not running in Basecamp"})
        try { return logos.callModule("pilot", method, args || []) }
        catch (e) { return JSON.stringify({error: e.message}) }
    }

    function refreshGlobal() {
        // Load existing pilot.db (from CLI deploy or previous Basecamp session)
        var initRaw = call("initialize", ["/tmp/pilot-data"])
        console.log("PILOT init result:", initRaw)

        var raw = call("metaStatus")
        console.log("PILOT metaStatus raw:", raw)
        console.log("PILOT metaStatus type:", typeof raw)

        try {
            var s = JSON.parse(raw)
            if (typeof s === "string") s = JSON.parse(s)
            console.log("PILOT parsed type:", typeof s, "initialized:", s.initialized, "account:", s.account, "owner:", s.owner_name)
            agentOnline = s.initialized || false
            accountId = s.account || ""
            ownerName = s.owner_name || ""
            if (s.llm && typeof s.llm === "object") {
                llmProvider = s.llm.provider || ""
                llmModel = s.llm.model || ""
            }
            console.log("PILOT agentOnline:", agentOnline, "accountId:", accountId, "ownerName:", ownerName)
            if (agentOnline || accountId !== "" || ownerName !== "") setupDone = true
            console.log("PILOT setupDone:", setupDone)
        } catch (e) {
            console.log("PILOT parse error:", e.message, "raw was:", raw)
        }
    }

    Component.onCompleted: refreshGlobal()

    Rectangle { anchors.fill: parent; color: Theme.palette.backgroundElevated }

    // ═══ Onboarding (first time, no CLI deploy) ═══
    SetupView {
        anchors.fill: parent
        visible: !root.setupDone
        onSetupComplete: {
            root.setupDone = true
            root.refreshGlobal()
        }
    }

    // ═══ Main app (after setup or CLI deploy) ═══
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: root.setupDone

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
                    text: "Pilot Agent"
                    font.pixelSize: Theme.typography.panelTitleText
                    font.weight: Theme.typography.weightBold
                    color: Theme.palette.text
                }

                Rectangle {
                    width: 8; height: 8; radius: Theme.spacing.radiusSmall
                    color: root.agentOnline ? Theme.palette.success : Theme.palette.error
                }
                LogosText {
                    text: root.agentOnline ? "Online" : "Offline"
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.textSecondary
                }

                Item { Layout.fillWidth: true }

                LogosText {
                    visible: root.ownerName !== ""
                    text: root.ownerName
                    font.pixelSize: Theme.typography.primaryText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }

                Rectangle {
                    visible: root.accountId !== ""
                    width: acctText.implicitWidth + Theme.spacing.medium * 2
                    height: 24
                    radius: Theme.spacing.radiusPill
                    color: Theme.palette.backgroundTertiary

                    LogosText {
                        id: acctText
                        anchors.centerIn: parent
                        text: root.accountId.length > 14
                            ? root.accountId.substring(0, 14) + "..."
                            : root.accountId
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textTertiary
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.palette.borderSecondary }

        // Tabs
        LogosTabBar {
            id: nav
            Layout.fillWidth: true
            LogosTabButton { text: "Dashboard" }
            LogosTabButton { text: "Chat" }
            LogosTabButton { text: "Wallet" }
            LogosTabButton { text: "Storage" }
            LogosTabButton { text: "Agent" }
        }

        // Content
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: nav.currentIndex

            DashboardView {}
            ChatView {}
            WalletView {}
            StorageView {}
            AgentView { onStatusRefreshed: root.refreshGlobal() }
        }
    }
}
