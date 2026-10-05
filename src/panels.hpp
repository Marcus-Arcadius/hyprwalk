#pragma once

#include "globals.hpp"
#include "math3d.hpp"

#if __has_include(<hyprland/src/output/Monitor.hpp>) // Hyprland 0.56 and later
#include <hyprland/src/output/Monitor.hpp>
#else
#include <hyprland/src/helpers/Monitor.hpp>
#endif
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Texture.hpp>

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace hyprwalk {

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
        WP<CWLSurfaceResource> surface; // null: the app's cursor, drawn over it
    };

    // where a panel is in the world
    struct SPanelPose {
        V3    origin;             // top-left corner
        V3    right{1, 0, 0};     // unit x and y axes (y down)
        V3    down{0, -1, 0};
        V3    normal{0, 0, 1};    // the side the content is visible from
        float scale = 1.f / 450;  // meters per logical px

        V3    at(const Vector2D& local) const {
            return origin + right * (float)(local.x * scale) + down * (float)(local.y * scale);
        }
        // panel-local logical px where a ray hits the panel's plane; false if parallel or behind the ray
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

    // a separate rectangle on the desktop: a window, layer surface or popup
    struct SPanel {
        uintptr_t    key  = 0; // stable id; keys GPU resources across frames
        ePanelKind   kind = PANEL_WINDOW;
        PHLWINDOWREF window; // the window, or the window owning a popup

        CBox         box;           // monitor-local logical coordinates
        int          layer    = -1; // zwlr layer for layer surfaces
        float        depth    = 0;  // meters in front of the wall once fully in 3D
        int          order    = 0;  // back to front drawing order on the 2D desktop
        float        rounding = 0;  // logical px
        float        alpha    = 1;
        bool         focused  = false;

        // hit testing: surface local = panelLocal * hitScale - hitOffset
        SP<CWLSurfaceResource> hitRoot;
        Vector2D               hitScale{1, 1};
        Vector2D               hitOffset{0, 0};

        std::vector<SPanelSurface> surfaces;

        // set after collecting, by whatever lays the panels out in the world
        SPanelPose pose;
        CBox       clip;           // visible part, panel-local logical px
        bool       placed = false; // somewhere in the world rather than on the desktop wall
        bool       held   = false; // being carried around
        bool       depthWrite = false; // occludes by depth, not drawing order
        bool       front    = false; // over everything (the played window and its popups)
        float      sortDist = 0;   // drawing order among placed panels (far first)
        uintptr_t  group    = 0;   // placed window it belongs to (itself or popup owner)
    };

    // the panels visible on a monitor, back to front on the wall; `spacing`: metres between stacking levels; windows in
    // `always` are included even when their workspace isn't shown; `inWorld`: 0 flat 2D .. 1 fully 3D
    std::vector<SPanel> collectPanels(PHLMONITOR mon, float spacing, const std::unordered_set<uintptr_t>& always = {}, float inWorld = 0.f);

    // owner of an X11 override-redirect window (menu, tooltip), null = none
    PHLWINDOW x11Owner(const PHLWINDOW& w);
}
