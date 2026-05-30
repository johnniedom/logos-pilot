import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: agentView

    signal statusRefreshed()

    property bool initialized: false
    property string account: ""
    property string ownerName: ""
    property string llmProvider: ""
    property string llmModel: ""
    property var skills: []
    property var agents: []
    property bool discovering: false
    property string configMsg: ""

    property var providers: [
        {label: "Anthropic (Claude)", key: "anthropic", models: ["claude-sonnet-4-6-20250514", "claude-opus-4-7-20250506", "claude-haiku-4-5-20251001"]},
        {label: "OpenAI (GPT)", key: "openai", models: ["gpt-4.1", "gpt-4.1-mini", "gpt-4o"]},
        {label: "DeepSeek", key: "deepseek", models: ["deepseek-v4-pro", "deepseek-v4-flash", "deepseek-chat"]},
        {label: "Google (Gemini)", key: "google", models: ["gemini-2.5-flash", "gemini-2.5-pro"]},
        {label: "OpenRouter", key: "openrouter", models: ["anthropic/claude-sonnet-4-6", "anthropic/claude-opus-4-7", "openai/gpt-4.1", "deepseek/deepseek-v4-pro", "google/gemini-2.5-flash"]},
        {label: "Groq (fast)", key: "groq", models: ["llama-3.3-70b-versatile", "llama-3.1-8b-instant"]}
    ]
    property var currentModels: []

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
            statusRefreshed()
        } catch (e) {}

        try {
            var sk = parseResult(call("metaSkills"))
            skills = sk.skills || []
        } catch (e) {}
    }

    function discover() {
        discovering = true
        try {
            var r = parseResult(call("agentDiscover", ["pilot"]))
            agents = r.agents || []
        } catch (e) {}
        discovering = false
    }

    function initAgent() {
        call("initialize", ["/tmp/pilot-data"])
        refresh()
    }

    function saveLlmConfig() {
        var provIdx = cfgProviderBox.currentIndex
        if (provIdx < 0 || provIdx >= providers.length) {
            configMsg = "Select a provider"
            return
        }
        var prov = providers[provIdx]
        var key = cfgApiKey.text.trim()
        if (!key) { configMsg = "Enter your API key"; return }

        call("metaConfigure", ["llm.provider", prov.key])
        call("metaConfigure", ["llm.api_key", key])

        var modelIdx = cfgModelBox.currentIndex
        if (modelIdx >= 0 && modelIdx < currentModels.length)
            call("metaConfigure", ["llm.model", currentModels[modelIdx]])

        configMsg = "Saved: " + prov.label
        refresh()
    }

    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: agentContent.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: agentContent
            width: flick.width
            spacing: Theme.spacing.large

            Item { Layout.preferredHeight: Theme.spacing.small }

            // ── Status cards ──
            GridLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                columns: 2
                columnSpacing: Theme.spacing.medium
                rowSpacing: Theme.spacing.medium

                // Status
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 80
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        spacing: Theme.spacing.tiny

                        LogosText {
                            text: "Status"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        RowLayout {
                            spacing: Theme.spacing.small
                            Rectangle {
                                width: 8; height: 8; radius: Theme.spacing.radiusSmall
                                color: agentView.initialized
                                    ? Theme.palette.success : Theme.palette.error
                            }
                            LogosText {
                                text: agentView.initialized ? "Initialized" : "Offline"
                                font.pixelSize: Theme.typography.primaryText
                                font.weight: Theme.typography.weightMedium
                                color: agentView.initialized
                                    ? Theme.palette.success : Theme.palette.error
                            }
                        }
                    }
                }

                // LLM
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 80
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        spacing: Theme.spacing.tiny

                        LogosText {
                            text: "LLM"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        LogosText {
                            text: agentView.llmProvider
                                ? agentView.llmProvider + " / " + agentView.llmModel
                                : "Not configured"
                            font.pixelSize: Theme.typography.primaryText
                            font.weight: Theme.typography.weightMedium
                            color: agentView.llmProvider
                                ? Theme.palette.text : Theme.palette.textTertiary
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                }

                // Account
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 80
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        spacing: Theme.spacing.tiny

                        LogosText {
                            text: "Agent Account"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        LogosText {
                            text: agentView.account || "—"
                            font.pixelSize: Theme.typography.primaryText
                            color: Theme.palette.text
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                    }
                }

                // Owner
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 80
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        spacing: Theme.spacing.tiny

                        LogosText {
                            text: "Owner"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        LogosText {
                            text: agentView.ownerName || "—"
                            font.pixelSize: Theme.typography.primaryText
                            font.weight: Theme.typography.weightMedium
                            color: Theme.palette.text
                        }
                    }
                }
            }

            // ── Initialize button (when offline) ──
            LogosButton {
                visible: !agentView.initialized
                Layout.leftMargin: Theme.spacing.large
                text: "Initialize Agent"
                onClicked: agentView.initAgent()
            }

            // ── LLM Settings ──
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                Layout.preferredHeight: cfgCol.implicitHeight + Theme.spacing.large * 2
                radius: Theme.spacing.radiusLarge
                color: Theme.palette.backgroundTertiary

                ColumnLayout {
                    id: cfgCol
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.large
                    spacing: Theme.spacing.small

                    LogosText {
                        text: "LLM Settings"
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.text
                    }

                    LogosText {
                        visible: agentView.llmProvider !== ""
                        text: "Current: " + agentView.llmProvider + " / " + agentView.llmModel
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textSecondary
                    }

                    LogosComboBox {
                        id: cfgProviderBox
                        Layout.fillWidth: true
                        model: {
                            var labels = []
                            for (var i = 0; i < agentView.providers.length; i++)
                                labels.push(agentView.providers[i].label)
                            return labels
                        }
                        currentIndex: -1
                        onCurrentIndexChanged: {
                            if (currentIndex >= 0 && currentIndex < agentView.providers.length)
                                agentView.currentModels = agentView.providers[currentIndex].models
                            else
                                agentView.currentModels = []
                        }
                    }

                    LogosComboBox {
                        id: cfgModelBox
                        Layout.fillWidth: true
                        visible: agentView.currentModels.length > 0
                        model: agentView.currentModels
                        currentIndex: 0
                    }

                    LogosTextField {
                        id: cfgApiKey
                        Layout.fillWidth: true
                        placeholderText: "API key"
                        echoMode: TextInput.Password
                    }

                    RowLayout {
                        spacing: Theme.spacing.medium
                        LogosButton {
                            text: "Save"
                            onClicked: agentView.saveLlmConfig()
                        }
                        LogosText {
                            visible: agentView.configMsg !== ""
                            text: agentView.configMsg
                            font.pixelSize: Theme.typography.secondaryText
                            color: agentView.configMsg.indexOf("Error") >= 0
                                ? Theme.palette.error : Theme.palette.success
                        }
                    }
                }
            }

            // ── Discovery ──
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                spacing: Theme.spacing.small

                RowLayout {
                    Layout.fillWidth: true
                    LogosText {
                        text: "Discovery"
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.text
                    }
                    Item { Layout.fillWidth: true }
                    LogosButton {
                        text: agentView.discovering ? "Searching..." : "Discover Agents"
                        enabled: !agentView.discovering
                        onClicked: agentView.discover()
                    }
                }

                LogosText {
                    visible: agentView.agents.length === 0
                    text: "No agents discovered yet"
                    font.pixelSize: Theme.typography.primaryText
                    color: Theme.palette.textTertiary
                }

                Repeater {
                    model: agentView.agents
                    Rectangle {
                        Layout.fillWidth: true
                        height: 48
                        radius: Theme.spacing.radiusMedium
                        color: Theme.palette.backgroundTertiary

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: Theme.spacing.medium

                            Rectangle {
                                width: 8; height: 8; radius: Theme.spacing.radiusSmall
                                color: Theme.palette.success
                            }
                            LogosText {
                                text: modelData.name || modelData.npk || "Agent"
                                font.pixelSize: Theme.typography.primaryText
                                color: Theme.palette.text
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                            }
                        }
                    }
                }
            }

            // ── Skills ──
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                spacing: Theme.spacing.small

                RowLayout {
                    Layout.fillWidth: true
                    LogosText {
                        text: "Skills (" + agentView.skills.length + ")"
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.text
                    }
                    Item { Layout.fillWidth: true }
                    LogosButton {
                        text: "Refresh"
                        onClicked: agentView.refresh()
                    }
                }

                Repeater {
                    model: agentView.skills

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        LogosText {
                            visible: index === 0 || agentView.skills[index].category !== agentView.skills[index - 1].category
                            text: (modelData.category || "").toUpperCase()
                            font.pixelSize: Theme.typography.secondaryText
                            font.weight: Theme.typography.weightBold
                            color: Theme.palette.primary
                            topPadding: index > 0 ? Theme.spacing.medium : 0
                            bottomPadding: Theme.spacing.tiny
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: skillRow.implicitHeight + Theme.spacing.small * 2
                            radius: Theme.spacing.radiusMedium
                            color: Theme.palette.backgroundTertiary

                            RowLayout {
                                id: skillRow
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spacing.medium
                                anchors.rightMargin: Theme.spacing.medium
                                spacing: Theme.spacing.medium

                                LogosText {
                                    text: modelData.name || "?"
                                    font.pixelSize: Theme.typography.primaryText
                                    font.weight: Theme.typography.weightMedium
                                    color: Theme.palette.text
                                    Layout.preferredWidth: 180
                                }
                                LogosText {
                                    text: modelData.description || ""
                                    font.pixelSize: Theme.typography.secondaryText
                                    color: Theme.palette.textSecondary
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                LogosText {
                                    visible: (modelData.price_lez || 0) > 0
                                    text: (modelData.price_lez || 0) + " LEZ"
                                    font.pixelSize: Theme.typography.secondaryText
                                    color: Theme.palette.warning
                                }
                            }
                        }
                    }
                }
            }

            Item { Layout.preferredHeight: Theme.spacing.xlarge }
        }
    }
}
