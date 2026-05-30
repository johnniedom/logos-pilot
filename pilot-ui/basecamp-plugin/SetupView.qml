import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: setup

    signal setupComplete()

    property int step: 0
    property string statusMsg: ""
    property bool deploying: false

    // Provider/model data (mirrors pilot-cli deploy.nim)
    property var providers: [
        {label: "Anthropic (Claude)", key: "anthropic", models: ["claude-sonnet-4-6-20250514", "claude-opus-4-7-20250506", "claude-haiku-4-5-20251001"]},
        {label: "OpenAI (GPT)", key: "openai", models: ["gpt-4.1", "gpt-4.1-mini", "gpt-4o"]},
        {label: "DeepSeek", key: "deepseek", models: ["deepseek-v4-pro", "deepseek-v4-flash", "deepseek-chat"]},
        {label: "Google (Gemini)", key: "google", models: ["gemini-2.5-flash", "gemini-2.5-pro"]},
        {label: "OpenRouter", key: "openrouter", models: ["anthropic/claude-sonnet-4-6", "anthropic/claude-opus-4-7", "openai/gpt-4.1", "deepseek/deepseek-v4-pro", "google/gemini-2.5-flash", "meta-llama/llama-4-maverick"]},
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

    function deploy() {
        deploying = true
        statusMsg = "Creating agent identity..."

        // Step 1: Initialize
        var initResult = call("initialize", ["/tmp/pilot-data"])
        try {
            var ir = parseResult(initResult)
            if (ir.error) { statusMsg = "Error: " + ir.error; deploying = false; return }
        } catch (e) {}

        // Step 2: Set owner name
        var name = nameField.text.trim()
        if (name) {
            call("metaConfigure", ["owner.name", name])
        }

        // Step 3: Configure LLM (if provider selected)
        var provIdx = providerBox.currentIndex
        if (provIdx >= 0 && provIdx < providers.length) {
            var prov = providers[provIdx]
            statusMsg = "Configuring " + prov.label + "..."

            call("metaConfigure", ["llm.provider", prov.key])

            var key = apiKeyField.text.trim()
            if (key) {
                call("metaConfigure", ["llm.api_key", key])
            }

            var modelIdx = modelBox.currentIndex
            if (modelIdx >= 0 && modelIdx < currentModels.length) {
                call("metaConfigure", ["llm.model", currentModels[modelIdx]])
            }
        }

        // Step 4: Publish agent card
        statusMsg = "Publishing Agent Card..."
        call("agentCard")

        statusMsg = "Done!"
        deploying = false
        setupComplete()
    }

    Rectangle { anchors.fill: parent; color: Theme.palette.backgroundElevated }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: col.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: col
            width: Math.min(flick.width, 520)
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Theme.spacing.large

            Item { Layout.preferredHeight: Theme.spacing.xxlarge }

            // ── Header ──
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                spacing: Theme.spacing.small

                LogosText {
                    text: "Welcome to Pilot"
                    font.pixelSize: Theme.typography.pageTitleText
                    font.weight: Theme.typography.weightBold
                    color: Theme.palette.text
                }
                LogosText {
                    text: "Set up your sovereign AI agent on the Logos Execution Zone."
                    font.pixelSize: Theme.typography.subtitleText
                    color: Theme.palette.textSecondary
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }

            // ── Your Name ──
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                Layout.preferredHeight: nameCol.implicitHeight + Theme.spacing.large * 2
                radius: Theme.spacing.radiusLarge
                color: Theme.palette.backgroundTertiary

                ColumnLayout {
                    id: nameCol
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.large
                    spacing: Theme.spacing.small

                    LogosText {
                        text: "Your Name"
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.text
                    }
                    LogosText {
                        text: "The agent will remember you by this name."
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textTertiary
                    }
                    LogosTextField {
                        id: nameField
                        Layout.fillWidth: true
                        placeholderText: "Enter your name"
                    }
                }
            }

            // ── LLM Provider ──
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                Layout.preferredHeight: llmCol.implicitHeight + Theme.spacing.large * 2
                radius: Theme.spacing.radiusLarge
                color: Theme.palette.backgroundTertiary

                ColumnLayout {
                    id: llmCol
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.large
                    spacing: Theme.spacing.small

                    LogosText {
                        text: "LLM Provider"
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.text
                    }
                    LogosText {
                        text: "Choose an AI model for natural language conversations. Skip for command-only mode."
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textTertiary
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    LogosComboBox {
                        id: providerBox
                        Layout.fillWidth: true
                        model: {
                            var labels = []
                            for (var i = 0; i < setup.providers.length; i++)
                                labels.push(setup.providers[i].label)
                            labels.push("Skip — command-only mode")
                            return labels
                        }
                        currentIndex: -1
                        onCurrentIndexChanged: {
                            if (currentIndex >= 0 && currentIndex < setup.providers.length) {
                                setup.currentModels = setup.providers[currentIndex].models
                                modelBox.currentIndex = 0
                            } else {
                                setup.currentModels = []
                            }
                        }
                    }

                    // Model selection (visible when provider chosen)
                    LogosComboBox {
                        id: modelBox
                        Layout.fillWidth: true
                        visible: setup.currentModels.length > 0
                        model: setup.currentModels
                        currentIndex: 0
                    }

                    // API key (visible when provider chosen, not skip)
                    ColumnLayout {
                        visible: providerBox.currentIndex >= 0
                            && providerBox.currentIndex < setup.providers.length
                        Layout.fillWidth: true
                        spacing: Theme.spacing.tiny

                        LogosText {
                            text: "API Key"
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                        }
                        LogosTextField {
                            id: apiKeyField
                            Layout.fillWidth: true
                            placeholderText: "sk-... or paste your key"
                            echoMode: TextInput.Password
                        }
                    }
                }
            }

            // ── Deploy button ──
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                spacing: Theme.spacing.small

                LogosButton {
                    text: setup.deploying ? "Deploying..." : "Deploy Agent"
                    enabled: !setup.deploying && nameField.text.trim().length > 0
                    Layout.fillWidth: true
                    onClicked: setup.deploy()
                }

                LogosText {
                    visible: setup.statusMsg !== ""
                    text: setup.statusMsg
                    font.pixelSize: Theme.typography.primaryText
                    color: setup.statusMsg.indexOf("Error") >= 0
                        ? Theme.palette.error : Theme.palette.primary
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }

            // ── Info footer ──
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

                    LogosText {
                        text: "Creates a shielded wallet on LEZ, generates keypair, stores in pilot.db"
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textTertiary
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
            }

            Item { Layout.preferredHeight: Theme.spacing.xxlarge }
        }
    }
}
