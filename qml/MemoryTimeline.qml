import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// What she remembers, by day and by episode, with the controls to keep or
// forget any of it. Privacy controls sit at the top, not in a sub-menu.
Window {
    id: root

    property var days: []
    property var detail: ({})
    property int selected: -1
    property string appFilter: ""
    property int dayFilter: 7

    objectName: "timelineWindow"
    title: "Nala — memories"
    width: 820
    height: 580
    minimumWidth: 640
    minimumHeight: 420
    flags: Qt.Dialog
    visible: false
    color: theme.colors.surface

    function refresh() {
        days = assistant.timeline(search.text, appFilter, dayFilter);
        apps.model = ["All apps"].concat(assistant.memoryApps());
        if (selected >= 0)
            detail = assistant.memoryDetail(selected);
    }
    function escaped(text) {
        return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
    }
    function select(id) {
        selected = id;
        detail = assistant.memoryDetail(id);
    }

    Component.onCompleted: refresh()
    onClosing: function (close) {
        close.accepted = false;
        hide();
    }

    Connections {
        target: screenMemory
        function onStored() {
            refreshLater.restart();
        }
    }
    Timer {
        id: refreshLater

        interval: 400
        onTriggered: root.refresh()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 12

        // Status and the switch that matters most.
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Rectangle {
                width: 10
                height: 10
                radius: 5
                color: screenMemory.recording ? "#e5484d" : theme.colors.muted
            }
            Label {
                textFormat: Text.PlainText
                                    text: "Screen memory is " + screenMemory.status + " · " + screenMemory.count + " memories · " + assistant.formatBytes(screenMemory.storageBytes)
                color: theme.colors.text
                font.family: theme.fontFamily
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            TButton {
                objectName: "timelinePause"
                visible: screenMemory.enabled
                text: screenMemory.paused ? "Resume" : "Pause"
                onClicked: screenMemory.paused ? screenMemory.resume() : screenMemory.pause(0)
            }
            TButton {
                text: "Forget last hour"
                onClicked: {
                    assistant.forgetMinutes(60);
                    root.refresh();
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TField {
                id: search

                objectName: "timelineSearch"
                Layout.fillWidth: true
                placeholderText: "Search memories"
                onTextChanged: refreshLater.restart()
            }
            TCombo {
                id: apps

                Layout.preferredWidth: 180
                model: ["All apps"]
                onActivated: {
                    root.appFilter = currentIndex === 0 ? "" : currentText;
                    root.refresh();
                }
            }
            TCombo {
                Layout.preferredWidth: 130
                model: [
                    { label: "Today", days: 1 },
                    { label: "7 days", days: 7 },
                    { label: "30 days", days: 30 },
                    { label: "Everything", days: 0 }
                ]
                textRole: "label"
                valueRole: "days"
                currentIndex: 1
                onActivated: {
                    root.dayFilter = currentValue;
                    root.refresh();
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 14

            // The timeline.
            ListView {
                id: list

                Layout.preferredWidth: 330
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: root.days

                delegate: Column {
                    id: dayItem

                    required property var modelData

                    width: list.width
                    spacing: 4

                    Label {
                        text: dayItem.modelData.label.toUpperCase()
                        color: theme.colors.muted
                        font.pixelSize: 11
                        font.bold: true
                        font.family: theme.fontFamily
                        topPadding: 8
                    }

                    Repeater {
                        model: dayItem.modelData.episodes

                        delegate: Rectangle {
                            id: episode

                            required property var modelData

                            width: dayItem.width
                            height: body.implicitHeight + 16
                            radius: 10 * theme.radius
                            color: root.selected === modelData.representative ? theme.colors.hover : theme.colors.card

                            Column {
                                id: body

                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.margins: 10
                                spacing: 2

                                Label {
                                    width: parent.width
                                    textFormat: Text.PlainText
                                    text: episode.modelData.time + "  ·  " + (episode.modelData.activity || episode.modelData.app) + (episode.modelData.pinned ? "  ·  pinned" : "")
                                    color: theme.colors.muted
                                    font.pixelSize: 11
                                    font.family: theme.fontFamily
                                    elide: Text.ElideRight
                                }
                                Label {
                                    width: parent.width
                                    textFormat: Text.PlainText
                                    text: episode.modelData.summary || episode.modelData.title
                                    color: theme.colors.text
                                    font.family: theme.fontFamily
                                    elide: Text.ElideRight
                                    maximumLineCount: 2
                                    wrapMode: Text.Wrap
                                }
                                Label {
                                    text: episode.modelData.count + (episode.modelData.count === 1 ? " memory" : " memories")
                                    color: theme.colors.muted
                                    font.pixelSize: 11
                                    font.family: theme.fontFamily
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                onClicked: root.select(episode.modelData.representative)
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: list.count === 0
                    width: list.width - 20
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: theme.colors.muted
                    font.family: theme.fontFamily
                    text: screenMemory.enabled ? "Nothing remembered here yet." : "Screen memory is off. Turn it on in Nala's preferences, under Memory, if you want her to remember what you work on."
                }
            }

            // The selected memory.
            Flickable {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentHeight: detailColumn.implicitHeight

                ColumnLayout {
                    id: detailColumn

                    width: parent.width
                    visible: root.detail.id !== undefined
                    spacing: 8

                    Image {
                        Layout.fillWidth: true
                        Layout.preferredHeight: status === Image.Ready ? width * sourceSize.height / Math.max(1, sourceSize.width) : 0
                        source: root.detail.image || ""
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                    }
                    Label {
                        visible: !root.detail.hasShot
                        text: "The screenshot has expired; the description is kept."
                        color: theme.colors.muted
                        font.family: theme.fontFamily
                    }
                    Label {
                        Layout.fillWidth: true
                        text: (root.detail.when || "") + "  ·  " + (root.detail.app || "")
                        color: theme.colors.muted
                        font.family: theme.fontFamily
                    }
                    Label {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                                    text: root.detail.title || ""
                        color: theme.colors.text
                        font.bold: true
                        font.family: theme.fontFamily
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        textFormat: Text.PlainText
                                    text: root.detail.summary || ""
                        color: theme.colors.text
                        font.family: theme.fontFamily
                        wrapMode: Text.Wrap
                    }
                    Repeater {
                        model: root.detail.artifacts || []

                        Label {
                            required property var modelData

                            Layout.fillWidth: true
                            text: "<a href=\"" + root.escaped(modelData.value) + "\">" + modelData.kind + ": " + root.escaped(modelData.value) + "</a>"
                            textFormat: Text.StyledText
                            linkColor: theme.colors.accent
                            elide: Text.ElideRight
                            font.family: theme.fontFamily
                            onLinkActivated: function (link) {
                                if (/^https?:\/\//.test(link))
                                    Qt.openUrlExternally(link);
                            }
                        }
                    }
                    Label {
                        visible: (root.detail.related || []).length > 0
                        text: "Around the same time"
                        color: theme.colors.muted
                        font.pixelSize: 11
                        font.bold: true
                        font.family: theme.fontFamily
                    }
                    Repeater {
                        model: root.detail.related || []

                        Label {
                            required property var modelData

                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                                    text: modelData.when + " · " + modelData.app + " · " + modelData.title
                            color: theme.colors.text
                            elide: Text.ElideRight
                            font.family: theme.fontFamily

                            MouseArea {
                                anchors.fill: parent
                                onClicked: root.select(parent.modelData.id)
                            }
                        }
                    }
                    RowLayout {
                        spacing: 8

                        TButton {
                            text: root.detail.pinned ? "Unpin" : "Pin"
                            onClicked: {
                                assistant.pinMemory(root.selected, !root.detail.pinned);
                                root.refresh();
                            }
                        }
                        TButton {
                            text: "Delete"
                            onClicked: {
                                assistant.forgetMemory(root.selected);
                                root.selected = -1;
                                root.detail = {};
                                root.refresh();
                            }
                        }
                    }
                }
            }
        }
    }

    component TButton: Button {
        id: tb

        implicitHeight: 32
        padding: 8
        leftPadding: 14
        rightPadding: 14
        background: Rectangle {
            radius: 12 * theme.radius
            color: tb.down ? theme.colors.pressed : tb.hovered ? theme.colors.hover : theme.colors.card
        }
        contentItem: Text {
            text: tb.text
            color: theme.colors.text
            font.family: theme.fontFamily
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    component TField: TextField {
        id: tf

        implicitHeight: 32
        leftPadding: 12
        color: theme.colors.text
        placeholderTextColor: theme.colors.muted
        font.family: theme.fontFamily
        font.pixelSize: 13
        background: Rectangle {
            radius: 12 * theme.radius
            color: theme.colors.card
            border.width: tf.activeFocus ? 1 : 0
            border.color: theme.colors.accent
        }
    }

    component TCombo: ComboBox {
        id: tc

        implicitHeight: 32
        contentItem: Text {
            leftPadding: 12
            text: tc.displayText
            color: theme.colors.text
            font.family: theme.fontFamily
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 12 * theme.radius
            color: tc.hovered ? theme.colors.hover : theme.colors.card
        }
    }
}
