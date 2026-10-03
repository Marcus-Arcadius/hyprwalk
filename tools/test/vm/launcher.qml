// launcher.qml: a launcher as a shell opens on a keybind: a full-screen layer surface on the overlay layer, see-through
// but for a box in the middle, that maps when shown and then takes the keyboard on demand (Quickshell's focusable
// PanelWindow); a click outside the box or Esc closes it. And a smaller one with exclusive keyboard focus, as rofi,
// fuzzel and wofi take. Typing and clicks go to /tmp/h3d-launcher.log, a line each.
//   quickshell -p launcher.qml
//   quickshell ipc -p launcher.qml call launcher toggle     (or: exclusive)
import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

ShellRoot {
    id: root
    property bool shown: false
    property bool exclusiveShown: false

    function say(line) {
        const p = Qt.createQmlObject('import Quickshell.Io; Process {}', root);
        p.command = ["sh", "-c", "printf '%s\\n' \"$1\" >> /tmp/h3d-launcher.log", "sh", line];
        p.running = true;
    }

    IpcHandler {
        target: "launcher"
        function toggle(): void {
            root.shown = !root.shown;
            root.say(root.shown ? "open" : "closed");
        }
        function exclusive(): void {
            root.exclusiveShown = !root.exclusiveShown;
            root.say(root.exclusiveShown ? "open exclusive" : "closed exclusive");
        }
    }

    PanelWindow {
        visible: root.shown
        focusable: root.shown
        WlrLayershell.namespace: "h3d-launcher"
        WlrLayershell.layer: WlrLayer.Overlay
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        anchors {
            top: true
            bottom: true
            left: true
            right: true
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {
                root.say("clicked outside");
                root.shown = false;
            }
        }

        Rectangle {
            anchors.centerIn: parent
            width: 480
            height: 260
            color: "#ff8800"

            MouseArea {
                anchors.fill: parent
                onClicked: m => root.say("clicked box " + Math.round(m.x) + " " + Math.round(m.y))
                onWheel: w => root.say("wheel " + (w.angleDelta.y > 0 ? "up" : "down"))
            }

            TextInput {
                id: input
                x: 20
                y: 20
                width: 440
                height: 60
                font.pixelSize: 32
                color: "black"
                onTextChanged: root.say("text " + text)
                Keys.onEscapePressed: {
                    root.say("escape");
                    root.shown = false;
                }
                Keys.onReturnPressed: root.say("enter " + text)
            }

            Rectangle {
                x: 20
                y: 180
                width: 200
                height: 60
                color: "#0044ff"
                MouseArea {
                    anchors.fill: parent
                    onClicked: m => root.say("clicked go " + Math.round(m.x) + " " + Math.round(m.y))
                }
            }
        }

        onVisibleChanged: if (visible) {
            input.text = "";
            input.forceActiveFocus();
        }
    }

    PanelWindow {
        visible: root.exclusiveShown
        WlrLayershell.namespace: "h3d-exclusive"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive
        exclusionMode: ExclusionMode.Ignore
        implicitWidth: 400
        implicitHeight: 160
        color: "#00cc66"

        TextInput {
            id: exclusiveInput
            x: 20
            y: 20
            width: 360
            height: 60
            font.pixelSize: 32
            color: "black"
            onTextChanged: root.say("exclusive text " + text)
            Keys.onEscapePressed: {
                root.say("exclusive escape");
                root.exclusiveShown = false;
            }
        }

        onVisibleChanged: if (visible) {
            exclusiveInput.text = "";
            exclusiveInput.forceActiveFocus();
        }
    }
}
