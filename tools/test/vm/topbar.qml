// topbar.qml: a shell's bar on the top layer (as a desktop's status bar is), orange, 36 px, across the top
//   quickshell -p topbar.qml
import QtQuick
import Quickshell
import Quickshell.Wayland

ShellRoot {
    Variants {
        model: Quickshell.screens

        delegate: Component {
            PanelWindow {
                required property var modelData
                screen: modelData

                WlrLayershell.namespace: "h3d-topbar"
                WlrLayershell.layer: WlrLayer.Top
                color: "#ff8800"
                anchors {
                    top: true
                    left: true
                    right: true
                }
                implicitHeight: 36
            }
        }
    }
}
