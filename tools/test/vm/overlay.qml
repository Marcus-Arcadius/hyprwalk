// overlay.qml: a shell's overlay over the whole screen, as quickshell shells have one (see-through, over everything,
// taking input only where it has something: here a band down the middle, as a dock or a hot edge would). Where it
// takes no input, clicks go through to what's behind it, on the 2D desktop and in 3D alike.
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

                WlrLayershell.namespace: "h3d-overlay"
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
