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
    width: 420
    height: Math.min(560, Screen.height - 80)
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
