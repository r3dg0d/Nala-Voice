import QtQuick
import QtQuick.Window
import Nala

Window {
    id: root

    // The body occupies the middle 1/1.92 of the window; the rest is headroom
    // for morphs and orbit rings that reach past the idle silhouette.
    readonly property real canvasToBody: 1.89
    property bool ready: false

    objectName: "mascotWindow"
    title: "Nala"
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
    color: "transparent"
    // Shown by main() once the surface role has been set up: LayerShellQt can
    // only convert a window before its platform surface is created.
    visible: false

    // One clock for the whole companion: the behaviour model and the orbit
    // simulation advance from the same frame callback, so they never drift.
    FrameAnimation {
        // The self-test drives the clock itself so behaviour is deterministic;
        // a live frame loop would race with it.
        running: !backend.testing
        onTriggered: {
            const dt = Math.min(frameTime, 0.1);
            mascot.tick(dt);
            backend.advance(dt);
            orbits.intensity = mascot.rings;
            orbits.advance(dt);
        }
    }

    Item {
        id: stage

        // Square and centred: the mascot's geometry is defined in a unit
        // square, so a non-square window must letterbox rather than stretch.
        anchors.centerIn: parent
        width: Math.min(parent.width, parent.height)
        height: width

        // Idle float, drag lean and the shake after an alert. Applied as a
        // transform so it does not fight the fill anchor.
        transform: Translate {
            x: mascot.bobX * stage.width * 0.5
            y: mascot.bobY * stage.height * 0.5
        }

        OrbitLayer {
            anchors.fill: parent
            source: orbits
            front: false
            scale2: mascot.bodyScale
        }

        ShaderEffect {
            id: body

            objectName: "bodyShader"
            anchors.fill: parent
            fragmentShader: "qrc:/shaders/mascot.frag.qsb"
            blending: true

            property color bodyColor: backend.mascotColor
            property color eyeColor: "#fdfdfd"
            property color badgeColor: "#3ea1f5"

            property real formA: mascot.formA
            property real formB: mascot.formB
            property real formMix: mascot.formMix

            property real squashX: mascot.squashX
            property real squashY: mascot.squashY
            property real bodyScale: mascot.bodyScale
            property vector4d bodyTransform: mascot.bodyTransform

            property vector2d eyeLeft: Qt.vector2d(mascot.eyeLeftX, mascot.eyeLeftY)
            property vector2d eyeRight: Qt.vector2d(mascot.eyeRightX, mascot.eyeRightY)
            property vector2d eyeLeftScale: Qt.vector2d(mascot.eyeLeftScaleX, mascot.eyeLeftScaleY)
            property vector2d eyeRightScale: Qt.vector2d(mascot.eyeRightScaleX, mascot.eyeRightScaleY)
            property real eyeLeftAngle: mascot.eyeLeftAngle
            property real eyeRightAngle: mascot.eyeRightAngle
            property real eyeWidth: mascot.eyeWidth
            property real eyeHeight: mascot.eyeHeight
            property real eyeRound: mascot.eyeRound

            property real badge: mascot.badge
            property real dotsSpread: mascot.dotsSpread
            property real dotsShrink: mascot.dotsShrink
            property real dotsPhase: mascot.dotsPhase
        }

        OrbitLayer {
            anchors.fill: parent
            source: orbits
            front: true
            scale2: mascot.bodyScale
        }

        Trail {
            anchors.fill: parent
            angle: mascot.dashAngle
            length: mascot.dashLength
            intensity: mascot.dashIntensity
            z: -1
        }

        // Droplets thrown clear when she comes apart. Positions arrive in
        // units of her body radius, measured from her centre.
        Repeater {
            model: mascot.droplets

            delegate: Rectangle {
                required property var modelData

                readonly property real unit: stage.width * 0.5 * 0.529

                color: backend.mascotColor
                opacity: modelData.opacity
                width: Math.max(1, modelData.radius * 2 * unit)
                height: width
                radius: width / 2
                antialiasing: true
                x: stage.width / 2 + modelData.x * unit - width / 2
                y: stage.height / 2 - modelData.y * unit - height / 2
            }
        }
    }

    MouseArea {
        id: pointer

        // Only the body area is interactive; the window mask already makes the
        // corners click-through, and this keeps hover honest.
        anchors.centerIn: parent
        width: parent.width / root.canvasToBody
        height: parent.height / root.canvasToBody

        // Where the press landed, in this item's own coordinates. The window
        // chases the pointer, so the delta from this point is the movement --
        // no global coordinates needed, which a Wayland client cannot trust.
        property point origin: Qt.point(0, 0)
        property bool moved: false

        // Under test the desktop's real pointer must not perturb her state.
        enabled: !backend.testing
        hoverEnabled: !backend.testing
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.PointingHandCursor

        onEntered: mascot.setHovered(true)
        onExited: mascot.setHovered(false)

        onPressed: function (mouse) {
            if (mouse.button === Qt.RightButton) {
                backend.openSettings();
                return;
            }
            origin = Qt.point(mouse.x, mouse.y);
            moved = false;
            backend.grabDrag();
        }

        onPositionChanged: function (mouse) {
            if (!backend.dragging())
                return;
            const dx = mouse.x - origin.x;
            const dy = mouse.y - origin.y;
            // A few pixels of slop so a click is never mistaken for a drag.
            if (!moved && Math.abs(dx) + Math.abs(dy) < 4)
                return;
            moved = true;
            backend.dragBy(dx, dy);
        }

        onReleased: function (mouse) {
            if (mouse.button !== Qt.LeftButton)
                return;
            backend.releaseDrag();
            if (!moved)
                mascot.poke();
        }

        // If the compositor takes the grab away, do not strand her mid-drag.
        onCanceled: backend.releaseDrag()

        onDoubleClicked: mascot.think(3.4)
        onWheel: mascot.wake()
    }

    Loader {
        id: settings

        active: false
        source: "qrc:/qml/Settings.qml"
        onLoaded: {
            item.show();
            item.raise();
            item.requestActivate();
        }
    }

    Connections {
        target: backend
        function onSettingsRequested() {
            if (!settings.active) {
                settings.active = true;
            } else {
                settings.item.show();
                settings.item.raise();
                settings.item.requestActivate();
            }
        }
    }
}
