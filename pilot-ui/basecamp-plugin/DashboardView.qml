import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: dashboard

    property string balance: "—"
    property string account: ""
    property bool initialized: false
    property string llmProvider: ""
    property string llmModel: ""
    property string ownerName: ""
    property int fileCount: 0
    property int pendingCount: 0
    property int skillCount: 0

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
            var s = parseResult(call("metaStatus"))
            initialized = s.initialized || false
            account = s.account || ""
            ownerName = s.owner_name || ""
            if (s.llm && typeof s.llm === "object") {
                llmProvider = s.llm.provider || ""
                llmModel = s.llm.model || ""
            }
        } catch (e) {}

        try {
            var br = parseResult(call("walletBalance"))
            if (!br.error) balance = br.balance || "0"
        } catch (e) {}

        try {
            var fl = parseResult(call("storageList"))
            fileCount = (fl.files || []).length
        } catch (e) {}

        try {
            var pr = parseResult(call("getPendingSpends"))
            pendingCount = (pr.pending || []).length
        } catch (e) {}

        try {
            var sk = parseResult(call("metaSkills"))
            skillCount = sk.count || 0
        } catch (e) {}
    }

    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: col.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: col
            width: flick.width
            spacing: Theme.spacing.large

            Item { Layout.preferredHeight: Theme.spacing.small }

            // ── Welcome header ──
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                spacing: Theme.spacing.tiny

                LogosText {
                    text: dashboard.ownerName
                        ? "Welcome back, " + dashboard.ownerName
                        : "Pilot Agent"
                    font.pixelSize: Theme.typography.pageTitleText
                    font.weight: Theme.typography.weightBold
                    color: Theme.palette.text
                }
                LogosText {
                    text: "Sovereign AI agent on LEZ"
                    font.pixelSize: Theme.typography.subtitleText
                    color: Theme.palette.textSecondary
                }
            }

            // ── Pending alert ──
            Rectangle {
                visible: dashboard.pendingCount > 0
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                Layout.preferredHeight: 48
                radius: Theme.spacing.radiusLarge
                color: Theme.palette.backgroundTertiary
                border.color: Theme.palette.warning
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacing.large
                    anchors.rightMargin: Theme.spacing.large

                    LogosText {
                        text: dashboard.pendingCount + " pending approval" + (dashboard.pendingCount > 1 ? "s" : "")
                        font.pixelSize: Theme.typography.primaryText
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.warning
                    }
                    Item { Layout.fillWidth: true }
                    LogosText {
                        text: "Go to Wallet tab to review"
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textSecondary
                    }
                }
            }

            // ── Primary stats ──
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                spacing: Theme.spacing.medium

                // Balance
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 120
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.large
                        spacing: Theme.spacing.small

                        LogosText {
                            text: "Agent Wallet"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        LogosText {
                            text: dashboard.balance + " LEZ"
                            font.pixelSize: Theme.typography.pageTitleText
                            font.weight: Theme.typography.weightBold
                            color: Theme.palette.text
                        }
                        LogosText {
                            text: dashboard.account
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textTertiary
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                    }
                }

                // Agent status
                Rectangle {
                    Layout.preferredWidth: 180
                    Layout.preferredHeight: 120
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.large
                        spacing: Theme.spacing.small

                        LogosText {
                            text: "Agent"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        RowLayout {
                            spacing: Theme.spacing.small
                            Rectangle {
                                width: 10; height: 10; radius: 5
                                color: dashboard.initialized
                                    ? Theme.palette.success : Theme.palette.error
                            }
                            LogosText {
                                text: dashboard.initialized ? "Online" : "Offline"
                                font.pixelSize: Theme.typography.subtitleText
                                font.weight: Theme.typography.weightBold
                                color: dashboard.initialized
                                    ? Theme.palette.success : Theme.palette.error
                            }
                        }
                        LogosText {
                            text: dashboard.skillCount + " skills"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textTertiary
                        }
                    }
                }
            }

            // ── Secondary stats grid ──
            GridLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                columns: 3
                columnSpacing: Theme.spacing.medium
                rowSpacing: Theme.spacing.medium

                // LLM
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 88
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        spacing: Theme.spacing.tiny

                        LogosText {
                            text: "LLM Provider"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        LogosText {
                            text: dashboard.llmProvider || "None"
                            font.pixelSize: Theme.typography.primaryText
                            font.weight: Theme.typography.weightBold
                            color: dashboard.llmProvider
                                ? Theme.palette.primary : Theme.palette.textTertiary
                        }
                        LogosText {
                            visible: dashboard.llmModel !== ""
                            text: dashboard.llmModel
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textTertiary
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                }

                // Files
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 88
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        spacing: Theme.spacing.tiny

                        LogosText {
                            text: "Encrypted Files"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        LogosText {
                            text: dashboard.fileCount.toString()
                            font.pixelSize: Theme.typography.panelTitleText
                            font.weight: Theme.typography.weightBold
                            color: Theme.palette.text
                        }
                        LogosText {
                            text: "AES-256-GCM"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textTertiary
                        }
                    }
                }

                // Pending
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 88
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        spacing: Theme.spacing.tiny

                        LogosText {
                            text: "Pending Spends"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        LogosText {
                            text: dashboard.pendingCount.toString()
                            font.pixelSize: Theme.typography.panelTitleText
                            font.weight: Theme.typography.weightBold
                            color: dashboard.pendingCount > 0
                                ? Theme.palette.warning : Theme.palette.text
                        }
                        LogosText {
                            text: "9-state FSM"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textTertiary
                        }
                    }
                }
            }

            // ── Capabilities overview ──
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                spacing: Theme.spacing.small

                LogosText {
                    text: "Capabilities"
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightBold
                    color: Theme.palette.text
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 3
                    columnSpacing: Theme.spacing.small
                    rowSpacing: Theme.spacing.small

                    Repeater {
                        model: [
                            {name: "Wallet", desc: "Balance, send, history", accent: "#ED7B58"},
                            {name: "Storage", desc: "Upload, download, share", accent: "#4A90E2"},
                            {name: "Messaging", desc: "Send, join, groups", accent: "#8B5CF6"},
                            {name: "A2A Protocol", desc: "Discover, task, subscribe", accent: "#49F563"},
                            {name: "Blockchain", desc: "Query, call, deploy", accent: "#FB3748"},
                            {name: "Meta", desc: "Skills, status, configure", accent: "#A4A4A4"}
                        ]

                        Rectangle {
                            Layout.fillWidth: true
                            height: 64
                            radius: Theme.spacing.radiusMedium
                            color: Theme.palette.backgroundTertiary
                            border.color: modelData.accent
                            border.width: 1

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: Theme.spacing.medium
                                spacing: 2

                                LogosText {
                                    text: modelData.name
                                    font.pixelSize: Theme.typography.primaryText
                                    font.weight: Theme.typography.weightBold
                                    color: modelData.accent
                                }
                                LogosText {
                                    text: modelData.desc
                                    font.pixelSize: Theme.typography.secondaryText
                                    color: Theme.palette.textSecondary
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }
                }
            }

            // ── Network info ──
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                Layout.preferredHeight: 48
                radius: Theme.spacing.radiusMedium
                color: Theme.palette.backgroundTertiary

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacing.large
                    anchors.rightMargin: Theme.spacing.large
                    spacing: Theme.spacing.xlarge

                    LogosText {
                        text: "LEZ Testnet"
                        font.pixelSize: Theme.typography.primaryText
                        font.weight: Theme.typography.weightMedium
                        color: Theme.palette.text
                    }
                    LogosText {
                        text: "Waku Relay"
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textTertiary
                    }
                    LogosText {
                        text: "ECIES + AES-256-GCM"
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textTertiary
                    }
                    Item { Layout.fillWidth: true }
                    LogosText {
                        text: "LP-0008"
                        font.pixelSize: Theme.typography.primaryText
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.primary
                    }
                }
            }

            Item { Layout.preferredHeight: Theme.spacing.xlarge }
        }
    }
}
