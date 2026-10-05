#pragma once

#include "collision.hpp"
#include "math3d.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hyprwalk {

    enum eMaterial : uint8_t {
        MAT_SAND = 0,  // floor tiles
        MAT_STONE,     // sandstone blocks
        MAT_CRATE,     // wooden crates
        MAT_BEZEL,     // dark satin plastic (unused)
        MAT_PLASTER,   // smoother upper walls / trims
    };

    struct SVertex {
        float pos[3];
        float normal[3];
        float uv[2];
        float material;
        float ao;
    };

    // the screen (the 2D desktop) lives on the north wall, centered on x = 0
    struct SScreenSpec {
        float height  = 2.4f;  // meters the full monitor height maps to
        float centerY = 1.65f; // eye height, so the transition starts seamlessly
        float aspect  = 16.f / 9.f;
    };

    // where the windows hang when they aren't placed anywhere else
    struct SDesktopAnchor {
        V3    center{0, 1.65f, 0}; // middle of the desktop
        V3    normal{0, 0, 1};     // pointing out of the wall, horizontal
        float height = 2.4f;       // meters the monitor height maps to
    };

    struct SMapModel; // a loaded glTF map, see map.hpp

    // towards the sun in the courtyard (maps bring their own, see SWorld::sunDir)
    inline V3 sunDirection() {
        return normalize(V3{-0.42f, 0.78f, 0.46f});
    }

    struct SWorld {
        std::string                name = "courtyard";
        std::vector<SVertex>       vertices; // built-in map: triangle list with procedural materials
        std::shared_ptr<SMapModel> model;    // loaded maps
        CCollision                 collision;
        SAABB                      bounds;   // playable area
        V3                         spawn;    // feet position after the intro
        float                      spawnYaw = 0;
        SDesktopAnchor             desktop;
        V3                         sunDir = sunDirection();
    };

    SWorld buildWorld(const SScreenSpec& screen);
}
