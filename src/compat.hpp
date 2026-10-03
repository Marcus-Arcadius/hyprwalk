#pragma once

// Hyprland 0.55/0.56 compatibility: one API for both. 0.56 moved the window and monitor lists, monitor lookup, cursor
// warps, frame scheduling and window suspension from CCompositor to state trackers and controllers; the pointer manager
// and cursor shape overrides into namespace Pointer; fullscreen into Fullscreen::controller(), whose modes are no
// longer bit flags. Animated position and size became positionAnimation() / sizeAnimation(), fades alpha(); closing
// windows and layers fade out as snapshots, so nothing in the lists is fading out; renderSoftwareCursorsFor gained a
// flag. 0.56 is detected by its output/ headers (Monitor.hpp moved there).

#if __has_include(<hyprland/src/output/Monitor.hpp>)
#define H3D_HYPRLAND_056 1
#endif

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#ifdef H3D_HYPRLAND_056
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#else
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/managers/PointerManager.hpp>
#include <hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>
#endif

#include <vector>

namespace h3d::hypr {

#ifdef H3D_HYPRLAND_056
    namespace Cursor = ::Pointer::Cursor;
#else
    namespace Cursor = ::Cursor;
#endif

    // renderSoftwareCursorsFor (hooked) flags after the position: 0.56 added screencopy before forceRender
#ifdef H3D_HYPRLAND_056
    inline constexpr int SOFT_CURSOR_FLAGS = 2;
#else
    inline constexpr int SOFT_CURSOR_FLAGS = 1;
#endif

    // the windows, bottom to top (g_pCompositor->m_windows)
    inline const std::vector<PHLWINDOW>& windows() {
#ifdef H3D_HYPRLAND_056
        return Desktop::windowState()->windows();
#else
        return g_pCompositor->m_windows;
#endif
    }

    // the monitors that are on (g_pCompositor->m_monitors)
    inline const std::vector<PHLMONITOR>& monitors() {
#ifdef H3D_HYPRLAND_056
        return State::monitorState()->monitors();
#else
        return g_pCompositor->m_monitors;
#endif
    }

    // the pointer manager (g_pPointerManager)
    inline auto& pointer() {
#ifdef H3D_HYPRLAND_056
        return Pointer::mgr();
#else
        return g_pPointerManager;
#endif
    }

    // the monitor a point is on, else the nearest one (getMonitorFromVector)
    inline PHLMONITOR monitorAt(const Vector2D& at) {
#ifdef H3D_HYPRLAND_056
        return State::monitorState()->query().vec(at).run();
#else
        return g_pCompositor->getMonitorFromVector(at);
#endif
    }

    // the monitor the cursor is on (getMonitorFromCursor)
    inline PHLMONITOR monitorAtCursor() {
#ifdef H3D_HYPRLAND_056
        return monitorAt(pointer()->position());
#else
        return g_pCompositor->getMonitorFromCursor();
#endif
    }

    // warps the cursor and focuses the monitor there; force: even with cursor:no_warps (warpCursorTo)
    inline void warpCursor(const Vector2D& to, bool force) {
#ifdef H3D_HYPRLAND_056
        Pointer::pointerController()->warpTo(to, force);
#else
        g_pCompositor->warpCursorTo(to, force);
#endif
    }

    // schedules a frame on the monitor (scheduleFrameForMonitor)
    inline void scheduleFrame(const PHLMONITOR& mon) {
#ifdef H3D_HYPRLAND_056
        mon->scheduleFrame();
#else
        g_pCompositor->scheduleFrameForMonitor(mon);
#endif
    }

    // marks windows on hidden workspaces suspended and the rest not (updateSuspendedStates)
    inline void updateSuspendedStates() {
#ifdef H3D_HYPRLAND_056
        Desktop::globalWindowController()->updateSuspendedStates();
#else
        g_pCompositor->updateSuspendedStates();
#endif
    }

    // the surface at a point of one of these layers, where on it, and its layer (vectorToLayerSurface)
    inline SP<CWLSurfaceResource> layerSurfaceAt(const Vector2D& at, std::vector<PHLLSREF>* layers, Vector2D* local, PHLLS* found) {
#ifdef H3D_HYPRLAND_056
        return Desktop::viewState()->hitTest().layerSurfaceAt(at, layers, local, found);
#else
        return g_pCompositor->vectorToLayerSurface(at, layers, local, found);
#endif
    }

    // animated position and size of a window or layer (m_realPosition, m_realSize)
    template <typename T>
    inline PHLANIMVAR<Vector2D>& realPosition(const T& v) {
#ifdef H3D_HYPRLAND_056
        return v->positionAnimation();
#else
        return v->m_realPosition;
#endif
    }

    template <typename T>
    inline PHLANIMVAR<Vector2D>& realSize(const T& v) {
#ifdef H3D_HYPRLAND_056
        return v->sizeAnimation();
#else
        return v->m_realSize;
#endif
    }

    // a window or layer closed but still drawn as it fades out (m_fadingOut)
    template <typename T>
    inline bool fadingOut(const T& v) {
#ifdef H3D_HYPRLAND_056
        return false;
#else
        return v->m_fadingOut;
#endif
    }

    // a layer's or popup's opacity as it fades in and out (m_alpha)
    template <typename T>
    inline float alpha(const T& v) {
#ifdef H3D_HYPRLAND_056
        return v->alpha().value();
#else
        return v->m_alpha->value();
#endif
    }

    // fullscreen or maximized (isFullscreen())
    inline bool fullscreenOrMaximized(const PHLWINDOW& w) {
#ifdef H3D_HYPRLAND_056
        return Fullscreen::controller()->isFullscreen(w);
#else
        return w->isFullscreen();
#endif
    }

    // fullscreen, not just maximized (isEffectiveInternalFSMode(FSMODE_FULLSCREEN))
    inline bool fullscreen(const PHLWINDOW& w) {
#ifdef H3D_HYPRLAND_056
        return Fullscreen::controller()->isFullscreen(w, Fullscreen::FSMODE_FULLSCREEN);
#else
        return w->isEffectiveInternalFSMode(FSMODE_FULLSCREEN);
#endif
    }

    // the workspace's fullscreen or maximized window, if it has one (m_hasFullscreenWindow, getFullscreenWindow())
    inline PHLWINDOW fullscreenWindow(const PHLWORKSPACE& ws) {
#ifdef H3D_HYPRLAND_056
        return Fullscreen::controller()->getFullscreenWindow(ws);
#else
        return ws->m_hasFullscreenWindow ? ws->getFullscreenWindow() : nullptr;
#endif
    }

    // the workspace has a window fullscreen, not just maximized (m_hasFullscreenWindow, m_fullscreenMode)
    inline bool hasFullscreen(const PHLWORKSPACE& ws) {
#ifdef H3D_HYPRLAND_056
        return Fullscreen::controller()->hasFullscreen(ws) && Fullscreen::controller()->getFullscreenModes(ws).internal == Fullscreen::FSMODE_FULLSCREEN;
#else
        return ws->m_hasFullscreenWindow && ws->m_fullscreenMode == FSMODE_FULLSCREEN;
#endif
    }

    // a fullscreen window over it keeps input from it (isInputBlocked(INPUT_BLOCK_BELOW_FULLSCREEN))
    inline bool blockedBelowFullscreen(const PHLWINDOW& w) {
#ifdef H3D_HYPRLAND_056
        return w->isInputBlockedReasonAnyOf(Desktop::View::INPUT_BLOCK_BELOW_FULLSCREEN);
#else
        return w->isInputBlocked(Desktop::View::INPUT_BLOCK_BELOW_FULLSCREEN);
#endif
    }
}
