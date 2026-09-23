import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// Teaching her a wake phrase: say it a few times, talk normally for a bit,
// let her hear the room, and she learns it. Everything stays on this
// computer; the recordings can be deleted from Preferences at any time.
Window {
    id: root

    property string phrase: "hey " + assistant.assistantName.toLowerCase()
    readonly property int wanted: 6
    property string message: ""
    property bool done: false
    property bool succeeded: false

    objectName: "wakeTrainingWindow"
    title: "Train wake word"
    width: 440
    height: 560
    flags: Qt.Dialog
    visible: false
    color: theme.colors.surface

    function begin(text) {
        phrase = text;
        done = false;
        succeeded = false;
        message = "";
        assistant.startTraining(text);
        show();
        raise();
        requestActivate();
    }

    onClosing: function (close) {
        if (!assistant.trainingBusy)
            assistant.cancelTraining();
    }

    Connections {
        target: assistant
        function onTrainingSample(kind, index, ok, text) {
            root.message = text;
        }
        function onTrainingFinished(ok, text) {
            root.done = true;
            root.succeeded = ok;
            root.message = text;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 22
        spacing: 12

        Label {
            text: "Say “" + root.phrase + "”"
            font.pixelSize: 20
            font.weight: Font.DemiBold
            color: theme.colors.text
            font.family: theme.fontFamily
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: theme.colors.muted
            font.family: theme.fontFamily
            font.pixelSize: 12
            text: "Press Record, then say the phrase the way you normally would. Six times is best; vary it a little. Recordings stay on this computer."
        }

        // One row per recording: done, waiting, or listening now.
        Repeater {
            model: root.wanted

            RowLayout {
                required property int index

                readonly property bool complete: index < assistant.trainingSamples
                readonly property bool current: index === assistant.trainingSamples

                spacing: 10
                Label {
                    text: complete ? "●" : "○"
                    color: complete ? theme.colors.accent : theme.colors.muted
                    font.pixelSize: 16
                }
                Label {
                    text: "Recording " + (index + 1)
                    color: theme.colors.text
                    font.family: theme.fontFamily
                }
                Label {
                    text: complete ? "Complete" : current && assistant.recordingKind === "phrase" ? "Listening…" : "Waiting"
                    color: theme.colors.muted
                    font.family: theme.fontFamily
                    font.pixelSize: 12
                }
            }
        }

        // The microphone level, so it is obvious she hears you.
        Rectangle {
            Layout.fillWidth: true
            height: 6
            radius: 3
            color: theme.colors.card
            Rectangle {
                width: parent.width * assistant.micLevel
                height: parent.height
                radius: 3
                color: assistant.recordingKind !== "" ? "#e5484d" : theme.colors.accent
            }
        }

        RowLayout {
            spacing: 8
            Btn {
                objectName: "recordPhrase"
                text: assistant.recordingKind === "phrase" ? "Listening…" : "Record"
                enabled: !assistant.trainingBusy && assistant.recordingKind === "" && assistant.trainingSamples < 10
                onClicked: assistant.recordSample("phrase")
            }
            Label {
                text: assistant.trainingSamples + " recorded"
                color: theme.colors.muted
                font.family: theme.fontFamily
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: theme.colors.text
            font.family: theme.fontFamily
            font.pixelSize: 12
            text: "Optional, but it makes false wake-ups much rarer: read this aloud, then press Stop.\n“When I got home the kitchen was a mess, so I cleaned it before dinner. Next month we might visit the coast, if the weather stays nice.”"
        }
        RowLayout {
            spacing: 8
            Btn {
                text: assistant.recordingKind === "speech" ? "Stop" : assistant.trainingSpeech ? "Record again" : "Record talking"
                enabled: !assistant.trainingBusy && (assistant.recordingKind === "" || assistant.recordingKind === "speech")
                onClicked: assistant.recordingKind === "speech" ? assistant.stopRecording() : assistant.recordSample("speech")
            }
            Btn {
                text: assistant.recordingKind === "noise" ? "Quiet please…" : "Record the room (4 s)"
                enabled: !assistant.trainingBusy && assistant.recordingKind === ""
                onClicked: assistant.recordSample("noise")
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.message.length > 0
            textFormat: Text.PlainText
            text: root.message
            color: root.done && !root.succeeded ? theme.colors.error : theme.colors.text
            font.family: theme.fontFamily
        }

        Item {
            Layout.fillHeight: true
        }

        RowLayout {
            Layout.fillWidth: true
            Btn {
                text: root.done ? "Close" : "Cancel"
                enabled: !assistant.trainingBusy
                onClicked: root.close()
            }
            Item {
                Layout.fillWidth: true
            }
            BusyIndicator {
                running: assistant.trainingBusy
                visible: running
                implicitWidth: 28
                implicitHeight: 28
            }
            Btn {
                objectName: "trainWake"
                text: assistant.trainingBusy ? "Learning…" : "Train"
                accent: true
                visible: !root.done
                enabled: !assistant.trainingBusy && assistant.recordingKind === "" && assistant.trainingSamples >= 3
                onClicked: assistant.finishTraining()
            }
        }
    }

    component Btn: Button {
        id: b

        property bool accent: false

        implicitHeight: 32
        leftPadding: 14
        rightPadding: 14
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
