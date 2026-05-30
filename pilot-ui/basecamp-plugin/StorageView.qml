import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: storageView

    property var fileList: []
    property string uploadMsg: ""
    property string downloadMsg: ""

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

    function refreshFiles() {
        try {
            var r = parseResult(call("storageList"))
            fileList = r.files || []
        } catch (e) { fileList = [] }
    }

    function doUpload() {
        var path = uploadPath.text.trim()
        var label = uploadLabel.text.trim()
        if (!path || !label) { uploadMsg = "Enter both path and label"; return }
        uploadMsg = "Uploading..."
        var raw = call("storageUpload", [path, label])
        try {
            var r = parseResult(raw)
            if (r.error) { uploadMsg = "Error: " + r.error; return }
            uploadMsg = "Uploaded " + r.label + "  CID: " + r.cid
            uploadPath.text = ""
            uploadLabel.text = ""
            refreshFiles()
        } catch (e) { uploadMsg = raw }
    }

    function doDownload(label, cid) {
        var dest = downloadPath.text.trim()
        if (!dest) dest = "/tmp/" + label
        downloadMsg = "Downloading..."
        var raw = call("storageDownload", [cid, dest])
        try {
            var r = parseResult(raw)
            if (r.error) { downloadMsg = "Error: " + r.error; return }
            downloadMsg = "Downloaded to " + r.path
        } catch (e) { downloadMsg = raw }
    }

    Component.onCompleted: refreshFiles()
    onVisibleChanged: if (visible) refreshFiles()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.large
        spacing: Theme.spacing.large

        // ── Upload ──
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: uploadCol.implicitHeight + Theme.spacing.large * 2
            radius: Theme.spacing.radiusLarge
            color: Theme.palette.backgroundTertiary

            ColumnLayout {
                id: uploadCol
                anchors.fill: parent
                anchors.margins: Theme.spacing.large
                spacing: Theme.spacing.small

                LogosText {
                    text: "Upload"
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightBold
                    color: Theme.palette.text
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small

                    LogosTextField {
                        id: uploadPath
                        Layout.fillWidth: true
                        placeholderText: "/path/to/file"
                    }
                    LogosTextField {
                        id: uploadLabel
                        Layout.preferredWidth: 200
                        placeholderText: "label"
                    }
                    LogosButton {
                        text: "Upload"
                        onClicked: storageView.doUpload()
                    }
                }

                LogosText {
                    visible: storageView.uploadMsg !== ""
                    text: storageView.uploadMsg
                    font.pixelSize: Theme.typography.secondaryText
                    color: storageView.uploadMsg.indexOf("Error") >= 0
                        ? Theme.palette.error : Theme.palette.success
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
        }

        // ── Download destination ──
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: dlCol.implicitHeight + Theme.spacing.medium * 2
            radius: Theme.spacing.radiusMedium
            color: Theme.palette.backgroundTertiary

            RowLayout {
                id: dlCol
                anchors.fill: parent
                anchors.margins: Theme.spacing.medium
                spacing: Theme.spacing.small

                LogosText {
                    text: "Download to:"
                    font.pixelSize: Theme.typography.primaryText
                    color: Theme.palette.textSecondary
                }
                LogosTextField {
                    id: downloadPath
                    Layout.fillWidth: true
                    placeholderText: "/tmp/filename (leave blank for auto)"
                }
            }
        }

        LogosText {
            visible: storageView.downloadMsg !== ""
            text: storageView.downloadMsg
            font.pixelSize: Theme.typography.secondaryText
            color: storageView.downloadMsg.indexOf("Error") >= 0
                ? Theme.palette.error : Theme.palette.success
        }

        // ── File list header ──
        RowLayout {
            Layout.fillWidth: true
            LogosText {
                text: "Files (" + storageView.fileList.length + ")"
                font.pixelSize: Theme.typography.subtitleText
                font.weight: Theme.typography.weightBold
                color: Theme.palette.text
            }
            Item { Layout.fillWidth: true }
            LogosButton {
                text: "Refresh"
                onClicked: storageView.refreshFiles()
            }
        }

        // ── File list ──
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                anchors.fill: parent
                model: storageView.fileList
                spacing: Theme.spacing.small
                clip: true

                delegate: Rectangle {
                    width: ListView.view.width
                    height: 72
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundTertiary

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        spacing: Theme.spacing.medium

                        Rectangle {
                            width: 44; height: 44
                            radius: Theme.spacing.radiusLarge
                            color: Theme.palette.backgroundSecondary

                            LogosText {
                                anchors.centerIn: parent
                                text: "📄"
                                font.pixelSize: 20
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            LogosText {
                                text: modelData.label || "?"
                                font.pixelSize: Theme.typography.primaryText
                                font.weight: Theme.typography.weightMedium
                                color: Theme.palette.text
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            LogosText {
                                text: modelData.cid || ""
                                font.pixelSize: Theme.typography.secondaryText
                                color: Theme.palette.textTertiary
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }

                        LogosButton {
                            text: "Download"
                            onClicked: storageView.doDownload(modelData.label, modelData.cid)
                        }
                    }
                }
            }

            LogosText {
                visible: storageView.fileList.length === 0
                anchors.centerIn: parent
                text: "No files stored yet"
                font.pixelSize: Theme.typography.primaryText
                color: Theme.palette.textTertiary
            }
        }
    }
}
