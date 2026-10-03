#pragma once

#include "collision.hpp"
#include "math3d.hpp"

#include <cmath>
#include <functional>
#include <optional>

namespace h3d {

    // player collision box; steps up ledges up to STEP_HEIGHT (the test harness walks maps with it too)
    struct SWalker {
        static constexpr float RADIUS      = 0.3f;
        static constexpr float STEP_HEIGHT = 0.5f;
        // visible height (camera, avatar) follows the box critically damped, leading by SEEN_LEAD of the climb rate
        static constexpr float SEEN_W = 14.f, SEEN_AIR_W = 25.f, CLIMB_K = 3.f, SEEN_LEAD = 0.8f;
        // on landing, the visible height keeps this share of the fall speed
        static constexpr float LAND_SEEN = 0.25f;

        V3    feet, vel;
        bool  onGround = true;
        // smoothed feet height, its velocity, and the mean climb rate
        float seenY = NAN, seenV = 0, climb = 0;

        // collision test for the box at `feet`, `height` tall (the plugin's also keeps it out of windows)
        std::function<bool(const V3& feet, float height)> overlaps;

        // moves by vel * dt one axis at a time, zeroing blocked axes; sticks to stairs and slopes on the ground
        void move(float dt, float height, bool fly);
        V3   seen() const {
            return {feet.x, std::isfinite(seenY) ? seenY : feet.y, feet.z};
        }
        void moveAxis(int axis, float d, bool& blocked, float height, bool fly);
    };

    // where a foot can stand: the ground under x, z near height y (a stair, a slope)
    std::optional<float> groundUnder(const CCollision& col, float x, float z, float y);
}
