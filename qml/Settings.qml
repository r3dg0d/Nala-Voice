import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: settings

    readonly property int motion: backend.reducedMotion ? 0 : 120 * theme.motionScale
    readonly property real controlRadius: 12 * theme.radius
    readonly property real cardRadius: 16 * theme.radius

    objectName: "settingsWindow"
    title: "Nala"
    // Which tab is showing, and the last value a field would not take.
    property int page: 0
    property string rejected: ""

    function v(key) {
        return assistantSettings.values[key];
    }
    function put(key, value) {
        rejected = assistantSettings.set(key, value) ? "" : "That value was not accepted for " + key + ".";
    }

    width: 500
    height: Math.min(660, Screen.height - 80)
    minimumWidth: width
    maximumWidth: width
    minimumHeight: height
    maximumHeight: height
    flags: Qt.Dialog
    visible: false
    color: theme.colors.surface

    onClosing: function (close) {
        close.accepted = false;
        hide();
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 18

        RowLayout {
            Layout.fillWidth: true

            LabelText {
                text: "Nala"
                font.pixelSize: 22
                font.weight: Font.DemiBold
            }

            Item {
                Layout.fillWidth: true
            }

            ActionButton {
                objectName: "closeSettings"
                text: "×"
                Accessible.name: "Close preferences"
                onClicked: settings.hide()
            }
        }


        // Tabs.
        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Repeater {
                model: ["Nala", "Assistant", "Agent", "Memory", "Privacy", "Developer"]

                ActionButton {
                    required property int index
                    required property string modelData

                    objectName: "tab" + modelData
                    text: modelData
                    Layout.fillWidth: true
                    selected: settings.page === index
                    onClicked: settings.page = index
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: settings.page

            // --- the companion ---------------------------------------------
            ColumnLayout {
                spacing: 18

                GridLayout {
                    columns: 2
                    columnSpacing: 30
                    rowSpacing: 10
                    Layout.fillWidth: true

                    LabelText {
                        text: "Size"
                    }

                    RowLayout {
                        Layout.fillWidth: true

                        NalaSlider {
                            objectName: "sizeSlider"
                            Layout.fillWidth: true
                            from: 0.55
                            to: 2.2
                            stepSize: 0.05
                            value: backend.size
                            onMoved: backend.configure("size", value)
                            Accessible.name: "Mascot size"
                        }

                        LabelText {
                            text: Math.round(backend.size * 100) + "%"
                            color: theme.colors.muted
                            Layout.preferredWidth: 38
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    LabelText {
                        text: "Colour"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        ActionButton {
                            objectName: "colorInk"
                            text: "Ink"
                            Layout.fillWidth: true
                            selected: backend.colorMode === "ink"
                            onClicked: backend.configure("colorMode", "ink")
                        }

                        ActionButton {
                            objectName: "colorTheme"
                            text: "Wallpaper"
                            Layout.fillWidth: true
                            selected: backend.colorMode === "theme"
                            onClicked: backend.configure("colorMode", "theme")
                        }
                    }

                    LabelText {
                        text: "Display"
                    }

                    ComboBox {
                        id: displayPicker

                        objectName: "displayPicker"
                        Layout.fillWidth: true
                        implicitHeight: 32
                        leftPadding: 12
                        rightPadding: 30
                        model: ["Primary display"].concat(backend.screens)
                        currentIndex: backend.monitor.length ? Math.max(0, backend.screens.indexOf(backend.monitor) + 1) : 0
                        onActivated: backend.configure("monitor", currentIndex === 0 ? "" : backend.screens[currentIndex - 1])

                        contentItem: LabelText {
                            text: displayPicker.displayText
                            elide: Text.ElideRight
                        }

                        indicator: LabelText {
                            text: "⌄"
                            x: displayPicker.width - 24
                            y: (displayPicker.height - height) / 2
                        }

                        background: Rectangle {
                            radius: settings.controlRadius
                            color: displayPicker.hovered ? theme.colors.hover : theme.colors.card
                            border.width: displayPicker.activeFocus ? 1 : 0
                            border.color: theme.colors.accent
                        }

                        delegate: ItemDelegate {
                            id: displayOption

                            required property int index
                            required property string modelData

                            width: displayPicker.width - 12
                            implicitHeight: 34
                            highlighted: displayPicker.highlightedIndex === index

                            contentItem: LabelText {
                                text: displayOption.modelData
                            }

                            background: Rectangle {
                                radius: settings.controlRadius
                                color: displayOption.highlighted || displayOption.hovered ? theme.colors.hover : "transparent"
                            }
                        }

                        popup: Popup {
                            y: displayPicker.height + 4
                            width: displayPicker.width
                            padding: 6
                            implicitHeight: contentItem.implicitHeight + 12

                            background: Rectangle {
                                radius: settings.cardRadius
                                color: theme.colors.card
                                border.width: 1
                                border.color: theme.colors.outline
                            }

                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                model: displayPicker.popup.visible ? displayPicker.delegateModel : null
                                currentIndex: displayPicker.highlightedIndex
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    NalaSwitch {
                        objectName: "followToggle"
                        text: "Follow the cursor"
                        checked: backend.followCursor
                        onToggled: backend.configure("followCursor", checked)
                    }

                    NalaSwitch {
                        objectName: "desktopToggle"
                        text: "React to your desktop"
                        checked: backend.reactToDesktop
                        onToggled: backend.configure("reactToDesktop", checked)
                    }

                    NalaSwitch {
                        objectName: "anticsToggle"
                        text: "Idle antics"
                        checked: backend.idleAntics
                        onToggled: backend.configure("idleAntics", checked)
                    }

                    NalaSwitch {
                        objectName: "sleepToggle"
                        text: "Sleep when idle"
                        checked: backend.sleepWhenIdle
                        onToggled: backend.configure("sleepWhenIdle", checked)
                    }

                    NalaSwitch {
                        objectName: "topToggle"
                        text: "Stay above windows"
                        checked: backend.stayOnTop
                        onToggled: backend.configure("stayOnTop", checked)
                    }

                    NalaSwitch {
                        objectName: "motionToggle"
                        text: "Reduce motion"
                        checked: backend.reducedMotion
                        onToggled: backend.configure("reducedMotion", checked)
                    }

                    NalaSwitch {
                        objectName: "loginToggle"
                        text: "Start at login"
                        checked: backend.startAtLogin
                        onToggled: {
                            backend.setStartAtLogin(checked);
                            checked = Qt.binding(function () {
                                return backend.startAtLogin;
                            });
                        }
                    }
                }

                LabelText {
                    Layout.fillWidth: true
                    visible: backend.feedback.length > 0
                    text: backend.feedback
                    color: theme.colors.error
                    wrapMode: Text.Wrap
                }

                Item {
                    Layout.fillHeight: true
                }

                RowLayout {
                    Layout.fillWidth: true

                    ActionButton {
                        text: "Quit Nala"
                        onClicked: backend.quit()
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    ActionButton {
                        objectName: "resetPlace"
                        text: "Reset position"
                        onClicked: backend.resetPlace()
                    }
                }
            }

            // --- ears, brain and voice ---------------------------------------
            SettingsPage {
                Heading {
                    text: "Language model"
                }
                Note {
                    text: "Any OpenAI-compatible server: Ollama, llama.cpp, vLLM, LM Studio. A Qwen vision model is a good fit. Using: " + (assistant.model || "none yet")
                }
                SettingSwitch {
                    key: "llm.enabled"
                    text: "Use a language model"
                }
                SettingField {
                    key: "llm.endpoint"
                    label: "Endpoint"
                }
                SettingField {
                    key: "llm.model"
                    label: "Model"
                    placeholder: "first one the server lists"
                }
                SettingField {
                    key: "llm.apiKey"
                    label: "API key (only if the server wants one)"
                    secret: true
                }
                SettingSwitch {
                    key: "llm.vision"
                    text: "The model can see images"
                }
                SettingSwitch {
                    key: "llm.toolCalling"
                    text: "Let the model use tools"
                }

                Heading {
                    text: "Speech recognition (whisper.cpp)"
                }
                SettingSwitch {
                    key: "stt.enabled"
                    text: "Listen for speech"
                }
                SettingChoice {
                    key: "stt.activation"
                    label: "Listening"
                    options: [
                        { label: "Push to talk", value: "push" },
                        { label: "Wake word", value: "wake" },
                        { label: "Always", value: "always" }
                    ]
                }
                Note {
                    text: "Push to talk: bind a key to “nala listen”. Wake word: the microphone stays open and she answers to “Hey Nala”."
                }
                SettingChoice {
                    key: "stt.mode"
                    label: "Recogniser"
                    options: [
                        { label: "Automatic", value: "auto" },
                        { label: "whisper-server", value: "server" },
                        { label: "whisper-cli", value: "cli" }
                    ]
                }
                SettingField {
                    key: "stt.serverUrl"
                    label: "whisper-server address"
                }
                SettingField {
                    key: "stt.model"
                    label: "Model file for whisper-cli"
                    placeholder: "~/.local/share/nala/whisper/ggml-base.en.bin"
                }
                SettingField {
                    key: "stt.language"
                    label: "Language (en, de, … or auto)"
                }
                SettingChoice {
                    key: "stt.device"
                    label: "Microphone"
                    options: [{ label: "Default", value: "" }].concat(assistant.microphones.map(function (name) {
                        return { label: name, value: name };
                    }))
                }

                Heading {
                    text: "Voice (Fish Speech)"
                }
                SettingSwitch {
                    key: "tts.enabled"
                    text: "Speak replies"
                }
                SettingSwitch {
                    key: "tts.muted"
                    text: "Muted for now (bubbles only)"
                }
                SettingField {
                    key: "tts.endpoint"
                    label: "Fish Speech server"
                }
                SettingField {
                    key: "tts.referenceId"
                    label: "Voice (reference id)"
                    placeholder: "the server's default voice"
                }
                SettingField {
                    key: "tts.stylePrefix"
                    label: "Style prefix, e.g. (cheerful)"
                }
                SettingSwitch {
                    key: "tts.streaming"
                    text: "Start speaking before the audio is complete"
                }
                SettingChoice {
                    key: "tts.device"
                    label: "Speaker"
                    options: [{ label: "Default", value: "" }].concat(assistant.speakers.map(function (name) {
                        return { label: name, value: name };
                    }))
                }
                SettingSwitch {
                    key: "ui.speechBubbles"
                    text: "Show speech bubbles"
                }
            }

            // --- hands ----------------------------------------------------------
            SettingsPage {
                Heading {
                    text: "Computer use"
                }
                Note {
                    text: "Everything the model does goes through these tools, and every call is written to the log."
                }
                SettingSwitch {
                    key: "agent.enabled"
                    text: "Let her act on the computer"
                }
                SettingChoice {
                    key: "agent.confirm"
                    label: "Ask first"
                    options: [
                        { label: "Before anything", value: "everything" },
                        { label: "Before risky actions", value: "risky" },
                        { label: "Only before high-risk ones", value: "high" }
                    ]
                }
                Note {
                    text: "Writing files and running commands always need a yes, whatever this says."
                }
                SettingSwitch {
                    key: "agent.input"
                    text: "Mouse and keyboard (needs wtype, ydotool)"
                }
                SettingSwitch {
                    key: "agent.files"
                    text: "Read and write files"
                }
                SettingList {
                    key: "agent.fileRoots"
                    label: "Folders she may use (home if empty)"
                }
                SettingSwitch {
                    key: "agent.browser"
                    text: "Open web pages"
                }
                SettingSwitch {
                    key: "agent.shell"
                    text: "Run shell commands"
                }
                Note {
                    visible: settings.v("agent.shell") === true
                    color: theme.colors.error
                    text: "Commands run as you, with your permissions. She asks before each one; read it before saying yes."
                }
            }

            // --- memory ---------------------------------------------------------
            SettingsPage {
                Heading {
                    text: "Screen memory"
                }
                Note {
                    text: "When on, she keeps an occasional picture of the window you are using, so you can ask what you were doing. Nothing leaves this computer. Currently " + screenMemory.status + ", " + screenMemory.count + " memories, " + assistant.formatBytes(screenMemory.storageBytes) + "."
                }
                NalaSwitch {
                    objectName: "memoryToggle"
                    text: "Remember my screen"
                    checked: screenMemory.enabled
                    onToggled: screenMemory.setEnabled(checked)
                }
                RowLayout {
                    spacing: 6

                    ActionButton {
                        visible: screenMemory.enabled
                        text: screenMemory.paused ? "Resume" : "Pause now"
                        onClicked: screenMemory.paused ? screenMemory.resume() : screenMemory.pause(0)
                    }
                    ActionButton {
                        text: "Open memories"
                        onClicked: backend.openTimeline()
                    }
                    ActionButton {
                        text: "Forget last hour"
                        onClicked: assistant.forgetMinutes(60)
                    }
                }
                SettingChoice {
                    key: "memory.intervalSec"
                    label: "Look every"
                    options: [
                        { label: "5 seconds", value: 5 },
                        { label: "10 seconds", value: 10 },
                        { label: "30 seconds", value: 30 },
                        { label: "1 minute", value: 60 },
                        { label: "5 minutes", value: 300 }
                    ]
                }
                SettingChoice {
                    key: "memory.scope"
                    label: "Capture"
                    options: [
                        { label: "The focused window", value: "window" },
                        { label: "The whole monitor", value: "monitor" }
                    ]
                }
                SettingSwitch {
                    key: "memory.dedupe"
                    text: "Skip pictures that barely changed"
                }
                SettingChoice {
                    key: "memory.screenshotDays"
                    label: "Keep pictures for"
                    options: [
                        { label: "1 day", value: 1 },
                        { label: "7 days", value: 7 },
                        { label: "30 days", value: 30 },
                        { label: "90 days", value: 90 },
                        { label: "Forever", value: 0 }
                    ]
                }
                SettingChoice {
                    key: "memory.semanticDays"
                    label: "Keep descriptions for"
                    options: [
                        { label: "30 days", value: 30 },
                        { label: "90 days", value: 90 },
                        { label: "1 year", value: 365 },
                        { label: "Forever", value: 0 }
                    ]
                }
                SettingChoice {
                    key: "memory.maxStorageMB"
                    label: "Use at most"
                    options: [
                        { label: "1 GB", value: 1024 },
                        { label: "5 GB", value: 5120 },
                        { label: "10 GB", value: 10240 },
                        { label: "25 GB", value: 25600 }
                    ]
                }
                Note {
                    text: "Over the limit, the oldest pictures go first. Pinned memories are never removed automatically."
                }
                SettingSwitch {
                    key: "memory.describe"
                    text: "Describe each memory with the model (needs vision)"
                }
            }

            // --- privacy --------------------------------------------------------
            SettingsPage {
                Heading {
                    text: "Privacy"
                }
                Note {
                    text: "Say “Nala, pause screen memory” at any time; it works without the language model. A red dot on her means she is recording; a crossed-out eye means paused."
                }
                SettingSwitch {
                    key: "privacy.blockSensitive"
                    text: "Never keep logins, banking, private browsing or adult content"
                }
                SettingSwitch {
                    key: "privacy.visionFilter"
                    text: "Also ask the model to check each picture first"
                }
                SettingSwitch {
                    key: "memory.requireWindowInfo"
                    text: "Skip capture when she can't tell which window is open"
                }
                SettingList {
                    key: "privacy.excludedApps"
                    label: "Never remember these apps"
                }
                SettingList {
                    key: "privacy.excludedTitles"
                    label: "Never remember windows whose title contains"
                }
            }

            // --- developer ------------------------------------------------------
            SettingsPage {
                Heading {
                    text: "Setup"
                }
                Repeater {
                    model: assistant.setup

                    LabelText {
                        required property var modelData

                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        color: modelData.ok ? theme.colors.text : modelData.optional ? theme.colors.muted : theme.colors.error
                        text: (modelData.ok ? "✓ " : modelData.optional ? "– " : "✗ ") + modelData.name + ": " + modelData.detail
                    }
                }
                ActionButton {
                    text: "Check again"
                    onClicked: assistant.diagnose()
                }
                SettingSwitch {
                    key: "developer.debug"
                    text: "Debug logging (transcripts and prompts)"
                }
                Heading {
                    text: "Recent events"
                }
                Repeater {
                    model: eventLog.recent.slice(0, 60)

                    LabelText {
                        required property var modelData

                        Layout.fillWidth: true
                        font.pixelSize: 11
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: modelData.cat === "error" ? theme.colors.error : theme.colors.muted
                        text: String(modelData.t).substring(11, 19) + "  " + modelData.cat + "  " + modelData.event + (modelData.tool ? "  " + modelData.tool : "") + (modelData.reason ? "  " + modelData.reason : "")
                    }
                }
            }
        }

        LabelText {
            Layout.fillWidth: true
            visible: settings.rejected.length > 0
            text: settings.rejected
            color: theme.colors.error
            wrapMode: Text.Wrap
        }
    }

    component SettingsPage: Flickable {
        default property alias content: body.data

        clip: true
        contentWidth: width
        contentHeight: body.implicitHeight + 8
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: body

            width: parent.width - 14
            spacing: 6
        }
    }

    component Heading: LabelText {
        Layout.topMargin: 8
        font.pixelSize: 15
        font.weight: Font.DemiBold
    }

    component Note: LabelText {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        font.pixelSize: 12
    }

    component SettingSwitch: NalaSwitch {
        property string key

        checked: settings.v(key) === true
        onToggled: settings.put(key, checked)
    }

    component NalaField: TextField {
        id: field

        implicitHeight: 32
        leftPadding: 10
        color: theme.colors.text
        placeholderTextColor: theme.colors.muted
        font.family: theme.fontFamily
        font.pixelSize: 13
        background: Rectangle {
            radius: settings.controlRadius
            color: theme.colors.card
            border.width: field.activeFocus ? 1 : 0
            border.color: theme.colors.accent
        }
    }

    component SettingField: ColumnLayout {
        property string key
        property string label
        property string placeholder
        property bool secret: false

        Layout.fillWidth: true
        spacing: 2

        LabelText {
            text: parent.label
            color: theme.colors.muted
            font.pixelSize: 12
        }
        NalaField {
            Layout.fillWidth: true
            text: settings.v(parent.key) || ""
            placeholderText: parent.placeholder
            echoMode: parent.secret ? TextInput.Password : TextInput.Normal
            onEditingFinished: {
                if (text !== (settings.v(parent.key) || ""))
                    settings.put(parent.key, text);
            }
        }
    }

    component SettingList: ColumnLayout {
        property string key
        property string label

        Layout.fillWidth: true
        spacing: 2

        LabelText {
            text: parent.label + " (comma separated)"
            color: theme.colors.muted
            font.pixelSize: 12
        }
        NalaField {
            Layout.fillWidth: true
            text: (settings.v(parent.key) || []).join(", ")
            onEditingFinished: settings.put(parent.key, text.split(/[,\n]/).map(function (s) {
                return s.trim();
            }).filter(function (s) {
                return s.length > 0;
            }))
        }
    }

    component SettingChoice: RowLayout {
        id: choice

        property string key
        property string label
        property var options: []

        Layout.fillWidth: true

        LabelText {
            text: choice.label
            Layout.preferredWidth: 150
        }
        ComboBox {
            id: box

            Layout.fillWidth: true
            implicitHeight: 32
            model: choice.options
            textRole: "label"
            valueRole: "value"
            currentIndex: Math.max(0, indexOfValue(settings.v(choice.key)))
            onActivated: settings.put(choice.key, currentValue)

            contentItem: LabelText {
                leftPadding: 10
                text: box.displayText
                elide: Text.ElideRight
            }
            background: Rectangle {
                radius: settings.controlRadius
                color: box.hovered ? theme.colors.hover : theme.colors.card
            }
        }
    }

    component LabelText: Text {
        color: theme.colors.text
        font.pixelSize: 13
        font.family: theme.fontFamily
        verticalAlignment: Text.AlignVCenter
    }

    component ActionButton: Button {
        id: btn

        property bool selected: false

        implicitHeight: 32
        implicitWidth: Math.max(32, contentItem.implicitWidth + 22)
        padding: 8

        background: Rectangle {
            radius: settings.controlRadius
            color: btn.selected ? theme.colors.accent : btn.down ? theme.colors.pressed : btn.hovered ? theme.colors.hover : theme.colors.card
            border.width: btn.activeFocus ? 1 : 0
            border.color: theme.colors.accent

            Behavior on color {
                ColorAnimation {
                    duration: settings.motion
                }
            }
        }

        contentItem: LabelText {
            text: btn.text
            color: btn.selected ? theme.colors.onAccent : theme.colors.text
            opacity: btn.enabled ? 1 : 0.3
            horizontalAlignment: Text.AlignHCenter
        }
    }

    component NalaSlider: Slider {
        id: control

        implicitHeight: 30

        background: Rectangle {
            x: control.leftPadding
            y: control.topPadding + control.availableHeight / 2 - 2
            width: control.availableWidth
            height: 4
            radius: 2
            color: theme.colors.outline

            Rectangle {
                width: control.visualPosition * parent.width
                height: 4
                radius: 2
                color: theme.colors.accent
            }
        }

        handle: Rectangle {
            x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
            y: control.topPadding + control.availableHeight / 2 - height / 2
            width: 16
            height: 16
            radius: 8
            color: control.pressed ? theme.colors.text : theme.colors.accent
            border.width: control.activeFocus ? 2 : 0
            border.color: theme.colors.text

            Behavior on color {
                ColorAnimation {
                    duration: settings.motion
                    easing.type: Easing.OutCubic
                }
            }
        }
    }

    component NalaSwitch: Switch {
        id: control

        opacity: enabled ? 1 : 0.4
        Layout.fillWidth: true
        implicitHeight: 34
        padding: 0
        leftPadding: 0
        rightPadding: 48

        contentItem: LabelText {
            text: control.text
        }

        indicator: Rectangle {
            x: control.width - width
            y: (control.height - height) / 2
            width: 36
            height: 22
            radius: 11
            color: control.checked ? theme.colors.accent : theme.colors.outline
            border.width: control.activeFocus ? 1 : 0
            border.color: theme.colors.text

            Rectangle {
                x: control.checked ? 17 : 3
                y: 3
                width: 16
                height: 16
                radius: 8
                color: control.checked ? theme.colors.onAccent : theme.colors.muted

                Behavior on x {
                    NumberAnimation {
                        duration: settings.motion
                        easing.type: Easing.OutCubic
                    }
                }
            }

            Behavior on color {
                ColorAnimation {
                    duration: settings.motion
                }
            }
        }
    }
}
