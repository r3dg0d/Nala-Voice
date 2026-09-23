import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// What she says, what she heard, and what she is asking. A separate surface
// beside her, because her own window is only as big as she is; it takes no
// input unless there is a question to answer.
Window {
    id: bubble

    readonly property bool asking: assistant.question.length > 0
    readonly property bool listening: assistant.state === "listening"
    readonly property bool showing: assistantSettings.values["ui.speechBubbles"]
                                    && !backend.outOfTheWay
                                    && (assistant.bubble.length > 0 || asking || listening)
    readonly property int motion: backend.reducedMotion ? 0 : 160

    objectName: "bubbleWindow"
    title: "Nala says"
    width: 320
    height: 190
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool | Qt.WindowDoesNotAcceptFocus
    color: "transparent"
    // Shown by main() once it has been made a layer surface, like her.
    visible: false

    onAskingChanged: backend.setBubbleInteractive(asking)

    Rectangle {
        id: card

        objectName: "bubbleCard"
        width: parent.width - 8
        height: Math.min(parent.height - 14, column.implicitHeight + 24)
        x: 4
        // Hang towards her: from the bottom when she is below, the top when
        // she is above.
        y: backend.bubbleBelow ? 12 : parent.height - height - 12
        radius: 14 * theme.radius
        color: theme.colors.card
        border.width: 1
        border.color: bubble.asking ? theme.colors.accent : theme.colors.outline
        opacity: bubble.showing ? 1 : 0
        scale: bubble.showing ? 1 : 0.94
        transformOrigin: backend.bubbleBelow ? Item.Top : Item.Bottom

        Behavior on opacity {
            NumberAnimation {
                duration: bubble.motion
                easing.type: Easing.OutCubic
            }
        }
        Behavior on scale {
            NumberAnimation {
                duration: bubble.motion
                easing.type: Easing.OutBack
            }
        }

        // The tail, pointing at her.
        Rectangle {
            width: 12
            height: 12
            rotation: 45
            color: card.color
            border.width: card.border.width
            border.color: card.border.color
            x: backend.bubbleTail * card.width - width / 2
            y: backend.bubbleBelow ? -height / 2 : card.height - height / 2
            z: -1
        }

        ColumnLayout {
            id: column

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            spacing: 6

            // What she heard, small, so a misheard word is easy to spot.
            Text {
                Layout.fillWidth: true
                visible: assistant.heard.length > 0
                textFormat: Text.PlainText
                text: "“" + assistant.heard + "”"
                color: theme.colors.muted
                font.pixelSize: 11
                font.family: theme.fontFamily
                font.italic: true
                elide: Text.ElideRight
                maximumLineCount: 1
            }

            Text {
                objectName: "bubbleText"
                Layout.fillWidth: true
                visible: text.length > 0
                textFormat: Text.PlainText
                text: assistant.bubble
                color: theme.colors.text
                font.pixelSize: 14
                font.family: theme.fontFamily
                wrapMode: Text.Wrap
                maximumLineCount: 6
                elide: Text.ElideRight
            }

            // Listening: a small level meter, so it is obvious the mic is open.
            Row {
                visible: bubble.listening
                spacing: 3
                Layout.preferredHeight: 18

                Repeater {
                    model: 7

                    Rectangle {
                        required property int index

                        readonly property real reach: Math.max(0.15, assistant.micLevel * (1 - Math.abs(index - 3) / 4.5))

                        width: 4
                        height: 4 + 14 * reach
                        anchors.verticalCenter: parent.verticalCenter
                        radius: 2
                        color: theme.colors.accent
                    }
                }

                Text {
                    text: "  listening…"
                    color: theme.colors.muted
                    font.pixelSize: 12
                    font.family: theme.fontFamily
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // A question: say it, or click it.
            RowLayout {
                visible: bubble.asking
                Layout.fillWidth: true
                spacing: 8

                Item {
                    Layout.fillWidth: true
                }

                Button {
                    id: noButton

                    objectName: "answerNo"
                    text: "No"
                    onClicked: assistant.answer(false)
                    background: Rectangle {
                        radius: 10 * theme.radius
                        color: noButton.hovered ? theme.colors.hover : theme.colors.surface
                    }
                    contentItem: Text {
                        text: noButton.text
                        color: theme.colors.text
                        font.family: theme.fontFamily
                        horizontalAlignment: Text.AlignHCenter
                    }
                }

                Button {
                    id: yesButton

                    objectName: "answerYes"
                    text: "Yes"
                    onClicked: assistant.answer(true)
                    background: Rectangle {
                        radius: 10 * theme.radius
                        color: yesButton.hovered ? Qt.lighter(theme.colors.accent, 1.1) : theme.colors.accent
                    }
                    contentItem: Text {
                        text: yesButton.text
                        color: theme.colors.onAccent
                        font.family: theme.fontFamily
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }
        }
    }
}
