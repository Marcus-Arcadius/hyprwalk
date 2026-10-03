#pragma once

#include "collision.hpp"
#include "math3d.hpp"

#include <cmath>
#include <functional>
#include <optional>

namespace h3d {

    // The player's body going through the world (the plugin's, and the test harness's so it walks the maps the same):
    // a box RADIUS out each way and as tall as it stands, walking up and down ledges (stairs) up to STEP_HEIGHT, falling
    // off higher ones
    struct SWalker {
        static constexpr float RADIUS      = 0.3f;
        static constexpr float STEP_HEIGHT = 0.5f;
        // what's seen of its height (the camera, the avatar) follows it critically damped at SEEN_W a second, going on
        // at SEEN_LEAD of the mean speed it climbs at (the last 1/CLIMB_K seconds: hardly behind up a slope or a flight
        // of stairs, and past a single ledge only a little); in the air, along its arc at SEEN_AIR_W
        static constexpr float SEEN_W = 14.f, SEEN_AIR_W = 25.f, CLIMB_K = 3.f, SEEN_LEAD = 0.8f;
        // landing, what's seen gives this much of how fast the body came down (the rest it stops with it)
        static constexpr float LAND_SEEN = 0.25f;

        V3    feet, vel;
        bool  onGround = true;
        // its feet's height as it's seen: going up and down stairs smoothly, not a step at a time as the box goes (it
        // steps up as its edge reaches the next and down as it leaves one); how fast that goes, and the mean climb
        float seenY = NAN, seenV = 0, climb = 0;

        // what's solid: the box with its feet there, that tall (the plugin's also keeps it out of the windows)
        std::function<bool(const V3& feet, float height)> overlaps;

        // by its velocity for dt (gravity is the caller's): each way on its own, blocked ways' speed to 0; on the ground,
        // kept on it down a stair or a slope
        void move(float dt, float height, bool fly);
        V3   seen() const {
            return {feet.x, std::isfinite(seenY) ? seenY : feet.y, feet.z};
        }
        // along one axis (0 x, 1 y, 2 z) by d, stepping up a ledge on the ground; blocked: it stopped against something
        void moveAxis(int axis, float d, bool& blocked, float height, bool fly);
    };

    // where a foot can stand: the ground under x z, from a little above y to a little below (a stair, a slope)
    std::optional<float> groundUnder(const CCollision& col, float x, float z, float y);
}
