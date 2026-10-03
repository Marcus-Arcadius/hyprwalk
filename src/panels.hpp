#pragma once

#include "globals.hpp"
#include "math3d.hpp"

#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Texture.hpp>

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace h3d {

    enum ePanelKind : uint8_t {
        PANEL_LAYER = 0,
        PANEL_WINDOW,
        PANEL_POPUP,
    };

    // one wl_surface drawn into a panel
    struct SPanelSurface {
        SP<Render::ITexture>   tex;
        CBox                   box; // logical, relative to the panel's top-left
        Vector2D               uvTL{0, 0}, uvBR{1, 1};
        WP<CWLSurfaceResource> surface; // what it shows (none: the app's cursor, drawn over it)
    };

    // where a panel is in the world
    struct SPanelPose {
        V3    origin;             // top-left corner
        V3    right{1, 0, 0};     // unit vectors along the panel's x and y (y going down)
        V3    down{0, -1, 0};
        V3    normal{0, 0, 1};    // the side the content is visible from
        float scale = 1.f / 450;  // meters per logical px

        V3    at(const Vector2D& local) const {
            return origin + right * (float)(local.x * scale) + down * (float)(local.y * scale);
        }
        // panel-local logical px where a ray crosses the panel's plane; false when it
        // runs parallel or the plane is behind the ray
        bool intersect(const V3& eye, const V3& dir, float& t, Vector2D& local, bool frontOnly = true) const {
            const float denom = dot(dir, normal);
            if (frontOnly ? denom > -1e-5f : std::abs(denom) < 1e-5f)
                return false;
            t = dot(origin - eye, normal) / denom;
            if (t <= 0.f)
                return false;
            const V3 d = eye + dir * t - origin;
            local      = {dot(d, right) / scale, dot(d, down) / scale};
            return true;
        }
    };

    // Everything that is a separate rectangle on the desktop becomes a panel:
    // each window, each layer surface and each popup.
    struct SPanel {
        uintptr_t    key  = 0; // stable identity, used to keep GPU resources between frames
        ePanelKind   kind = PANEL_WINDOW;
        PHLWINDOWREF window; // the window, or the window owning a popup

        CBox         box;           // monitor-local logical coordinates
        int          layer    = -1; // zwlr layer for layer surfaces
        float        depth    = 0;  // meters in front of the wall once fully in 3D
        int          order    = 0;  // back to front drawing order on the 2D desktop
        float        rounding = 0;  // logical px
        float        alpha    = 1;
        bool         focused  = false;

        // input: the surface tree to hit-test, and how panel-local coordinates
        // map into it (surface local = panelLocal * hitScale - hitOffset)
        SP<CWLSurfaceResource> hitRoot;
        Vector2D               hitScale{1, 1};
        Vector2D               hitOffset{0, 0};

        std::vector<SPanelSurface> surfaces;

        // filled in after collecting, by whoever lays the panels out in the world
        SPanelPose pose;
        CBox       clip;           // visible part, panel-local logical px
        bool       placed = false; // somewhere in the world rather than on the desktop wall
        bool       held   = false; // being carried around
        bool       depthWrite = false; // hides what's behind it by depth instead of drawing order
        bool       front    = false; // drawn over everything, the world included (the window played, and its popups)
        float      sortDist = 0;   // drawing order among placed panels (far first)
        uintptr_t  group    = 0;   // the placed window this panel belongs to (itself or its popup's owner)
    };

    // collects the panels visible on a monitor, sorted back to front on the
    // desktop wall; `spacing` is the distance between stacking levels in meters.
    // Windows in `always` are included even when their workspace isn't shown.
    // `inWorld`: 0 on the flat 2D desktop .. 1 fully in 3D. In 3D a fullscreen (or maximized) window hides the rest of
    // its workspace only on the desktop wall, while it's on the wall itself: out in the world (in `always`) it covers
    // nothing there, and the windows out in the world are never under it
    std::vector<SPanel> collectPanels(PHLMONITOR mon, float spacing, const std::unordered_set<uintptr_t>& always = {}, float inWorld = 0.f);

    // the window an X11 override-redirect one (a menu, a tooltip) belongs to, null = none (a window of its own)
    PHLWINDOW x11Owner(const PHLWINDOW& w);
}
