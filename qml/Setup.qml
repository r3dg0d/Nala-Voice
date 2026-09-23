import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// First run: a few questions so nobody has to edit a config file. Every
// step can be skipped and changed later in Preferences.
Window {
    id: root

    property int step: 0
    readonly property int steps: 8

    objectName: "setupWindow"
    title: "Welcome"
    width: 520
    height: 520
    flags: Qt.Dialog
    visible: false
    color: theme.colors.surface

    function v(key) {
        return assistantSettings.values[key];
    }
    function next() {
        if (step === 2)
            assistant.stopMicTest();
        step = Math.min(steps - 1, step + 1);
        if (step === 2)
            assistant.startMicTest();
        if (step === 5)
            assistant.diagnose();
    }

    onClosing: {
        assistant.stopMicTest();
        assistant.finishSetup();
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 26
        spacing: 14

        Label {
            text: "Step " + (root.step + 1) + " of " + root.steps
            color: theme.colors.muted
            font.family: theme.fontFamily
            font.pixelSize: 11
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.step

            // 1. Who she is.
            Step {
                title: "Welcome to Nala."
                Body {
                    text: "What should I call your assistant?"
                }
                Field {
                    objectName: "setupName"
                    text: root.v("identity.name")
                    onEditingFinished: {
                        assistantSettings.set("identity.name", text.trim() || "Nala");
                        // Her wake phrase follows her name unless changed.
                        assistantSettings.set("wake.phrases", ["hey " + (text.trim() || "Nala").toLowerCase()]);
                    }
                }
            }

            // 2. What wakes her.
            Step {
                title: "How should she listen?"
                Body {
                    text: "With a wake phrase, a tiny detector on this computer listens only for it and throws everything else away."
                }
                Field {
                    text: (root.v("wake.phrases") || [])[0] || ""
                    placeholderText: "hey " + assistant.assistantName.toLowerCase()
                    onEditingFinished: assistantSettings.set("wake.phrases", [text.trim().toLowerCase()])
                }
                Choice {
                    options: [
                        { label: "Wake phrase", value: "wake" },
                        { label: "Push to talk only", value: "push" }
                    ]
                    current: root.v("stt.activation")
                    onPicked: function (value) {
                        assistantSettings.set("stt.activation", value);
                    }
                }
            }

            // 3. The microphone.
            Step {
                title: "Test your microphone"
                Body {
                    text: "Say something. The bar should move."
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 10
                    radius: 5
                    color: theme.colors.card
                    Rectangle {
                        width: parent.width * assistant.micLevel
                        height: parent.height
                        radius: 5
                        color: theme.colors.accent
                    }
                }
                Body {
                    text: assistant.microphones.length ? "Using: " + (root.v("stt.device") || "the default microphone") : "No microphone found. You can still type to her with “nala ask”."
                }
            }

            // 4. Teaching her the phrase.
            Step {
                title: "Teach her “" + ((root.v("wake.phrases") || [])[0] || "") + "”"
                Body {
                    text: assistant.wakeModelsPresent ? "Say it six times and she learns your voice. It takes about a minute, and the recordings never leave this computer." : "First, download the wake-word detector (about 190 MB; openWakeWord, free for personal use)."
                }
                Btn {
                    visible: !assistant.wakeModelsPresent
                    text: "Download"
                    onClicked: {
                        text = "Downloading…";
                        assistant.installWakeModels();
                    }
                }
                Btn {
                    visible: assistant.wakeModelsPresent
                    text: "Start training"
                    onClicked: training.item.begin((root.v("wake.phrases") || ["hey nala"])[0])
                }
                Body {
                    text: assistant.wakeStatus
                }
            }

            // 5. Her voice.
            Step {
                title: "Choose her voice"
                Body {
                    text: "Nala speaks through Fish Speech running on this computer. Without it she answers in a speech bubble."
                }
                Field {
                    text: root.v("tts.referenceId")
                    placeholderText: "Fish Speech voice (reference id), or empty for the default"
                    onEditingFinished: assistantSettings.set("tts.referenceId", text.trim())
                }
                Btn {
                    text: "Test voice"
                    onClicked: assistant.testVoice()
                }
            }

            // 6. Her brain.
            Step {
                title: "Connect local AI"
                Body {
                    text: "Qwen3.8-Flash-Next is recommended; any OpenAI-compatible server on this computer works (Ollama, llama.cpp, vLLM)."
                }
                Field {
                    text: root.v("llm.endpoint")
                    onEditingFinished: assistantSettings.set("llm.endpoint", text.trim())
                }
                Btn {
                    text: "Detect local server"
                    onClicked: assistant.diagnose()
                }
                Body {
                    text: assistant.model ? "Found: " + assistant.model + (assistant.model.toLowerCase().indexOf("flash-next") < 0 ? " (works; Qwen3.8-Flash-Next is the recommended model)" : "") : "Nothing found yet. Simple commands work without it."
                }
            }

            // 7. Screen memory.
            Step {
                title: "Screen memory"
                Body {
                    text: "She can keep an occasional picture of the window you are using, so you can later ask what you were doing. Private windows are never kept, and “pause screen memory” stops it instantly."
                }
                Choice {
                    options: [
                        { label: "Enable", value: true },
                        { label: "Not now", value: false }
                    ]
                    current: screenMemory.enabled
                    onPicked: function (value) {
                        screenMemory.setEnabled(value);
                    }
                }
            }

            // 8. Her hands.
            Step {
                title: "Computer control"
                Body {
                    text: "She can open apps, manage windows and look things up. Anything risky, she asks first; writing files and running commands always need your yes."
                }
                Choice {
                    options: [
                        { label: "Enable", value: true },
                        { label: "Not now", value: false }
                    ]
                    current: root.v("agent.enabled")
                    onPicked: function (value) {
                        assistantSettings.set("agent.enabled", value);
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Btn {
                text: "Skip setup"
                visible: root.step < root.steps - 1
                onClicked: root.close()
            }
            Item {
                Layout.fillWidth: true
            }
            Btn {
                text: "Back"
                visible: root.step > 0
                onClicked: root.step = root.step - 1
            }
            Btn {
                objectName: "setupNext"
                accent: true
                text: root.step === root.steps - 1 ? "Finish" : "Next"
                onClicked: root.step === root.steps - 1 ? root.close() : root.next()
            }
        }
    }

    Loader {
        id: training

        source: "qrc:/qml/WakeTraining.qml"
    }

    component Step: ColumnLayout {
        property string title

        spacing: 12
        Label {
            text: parent.title
            color: theme.colors.text
            font.family: theme.fontFamily
            font.pixelSize: 20
            font.weight: Font.DemiBold
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }
    }

    component Body: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        color: theme.colors.text
        font.family: theme.fontFamily
    }

    component Field: TextField {
        id: f

        Layout.fillWidth: true
        implicitHeight: 34
        leftPadding: 12
        color: theme.colors.text
        placeholderTextColor: theme.colors.muted
        font.family: theme.fontFamily
        background: Rectangle {
            radius: 12 * theme.radius
            color: theme.colors.card
            border.width: f.activeFocus ? 1 : 0
            border.color: theme.colors.accent
        }
    }

    component Choice: RowLayout {
        id: c

        property var options: []
        property var current
        signal picked(var value)

        spacing: 6
        Repeater {
            model: c.options
            Btn {
                required property var modelData
                text: modelData.label
                accent: c.current === modelData.value
                onClicked: {
                    c.current = modelData.value;
                    c.picked(modelData.value);
                }
            }
        }
    }

    component Btn: Button {
        id: b

        property bool accent: false

        implicitHeight: 34
        leftPadding: 16
        rightPadding: 16
        background: Rectangle {
            radius: 12 * theme.radius
            color: b.accent ? theme.colors.accent : b.down ? theme.colors.pressed : b.hovered ? theme.colors.hover : theme.colors.card
            opacity: b.enabled ? 1 : 0.4
        }
        contentItem: Text {
            text: b.text
            color: b.accent ? theme.colors.onAccent : theme.colors.text
            font.family: theme.fontFamily
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
