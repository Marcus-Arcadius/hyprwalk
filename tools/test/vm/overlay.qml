// overlay.qml: a shell's full-screen see-through overlay, as quickshell shells have, taking input only in a band down
// the middle (as a dock or a hot edge would); elsewhere clicks go through to what's behind, in 2D and 3D alike.
//   quickshell -p overlay.qml
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

                WlrLayershell.namespace: "hyprwalk-overlay"
                WlrLayershell.layer: WlrLayer.Overlay
                exclusionMode: ExclusionMode.Ignore
                color: "transparent"
                anchors {
                    top: true
                    bottom: true
                    left: true
                    right: true
                }
                mask: Region {
                    item: band
                }

                Rectangle {
                    id: band
                    x: parent.width / 2 - 20
                    width: 40
                    height: parent.height
                    color: "#60ff00ff"
                }
            }
        }
    }
}
