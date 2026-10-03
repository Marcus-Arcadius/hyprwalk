#pragma once

#include "math3d.hpp"

#include <vector>

namespace h3d {

    // Tiling mode's ring (T): windows side by side round where you stood, left to right in the order they're given,
    // upright and each turned to face the ring's center. Each is as big as on your screen, seen from where the view is
    // when you face it, made smaller to fit the view; the row is centred on where you looked, a little gap between two
    // windows, and when it would go further round you than `most`, all of it gets smaller alike. A window played in the
    // row (STileIn::fit) takes just the part of the view it's given, its middle where you look, and it's the others
    // that get smaller round it
    struct STileRing {
        V3    center;               // what they face: your eye (in third person the avatar's head)
        float yaw        = 0;       // the middle of the row, as the plugin has yaws (0 = -z, turning right is +)
        float radius     = 2.f;     // how far out from the center they stand, metres
        float back       = 0;       // how far behind the center the view is (third person: the camera's boom)
        float ground     = -1e9f;   // what they stand on: a window's bottom stays 5 cm above it
        float fit        = 0.85f;   // the most of the view's height and width one window takes
        float tanHalfFov = 0.7f;    // tan of half the view's height angle
        float aspect     = 16.f / 9; // the view's width over its height
        float monitorH   = 1440;    // the monitor's height, logical px: a window that tall fills the view, as on it
        float gap        = 0.04f;   // radians between two windows
        float most       = 5.93f;   // how far round you the row goes at the most, radians (some room stays behind you)
        float lookY      = NAN;     // how high your view's middle crosses the ring (ringLook): a window played in the row
                                    // has its middle there. NaN: level with the center
        float lookDist   = NAN;     // how far out that window's middle is from where the view is, level (your eye, or the
                                    // third-person camera where it is: in from its boom with a wall behind you), and the
        float lookPitch  = 0;       // view's pitch to it: it takes its fit of the view seen from there. NaN: back + radius,
                                    // level
    };

    struct STileIn {
        float w = 0, h = 0; // logical px
        float fit = 0;      // a window played in the row (plugin:hypr3d:play_size): just that much of the view's height or
                            // width, whichever it fills first, bigger or smaller than on your screen and whatever the
                            // ring's fit is; its middle at lookY, not standing on the ground (it's drawn over the world
                            // while it's played), and when the row would go too far round you, the others get smaller,
                            // not it (unless even with them at nothing it would: hardly ever). 0 = as the ring has it
    };

    struct STileOut {
        V3    center;            // its middle
        V3    right, up, normal; // its axes: normal level, towards the ring's center
        float scale = 0;         // metres per logical px
        float angle = 0;         // the yaw of its middle, seen from the ring's center
        float half  = 0;         // half the angle it takes round the ring
    };

    // the row, left to right
    std::vector<STileOut> layoutRing(const STileRing& ring, const std::vector<STileIn>& windows);

    // how much of the view a window laid out `scale` metres a px takes, seen as the window played in the row is (lookDist
    // out, pitched lookPitch): its height's share or its width's, the more
    float viewShare(const STileRing& ring, const STileIn& w, float scale);

    // where in the row laid out something at yaw `at` (seen from the ring's center) goes: after every window whose
    // middle is left of it, round from the row's middle, or right at it (a window played here is turned to where you
    // look: one opening there goes after it); 0 = before the first .. n = after the last
    int ringSlot(const STileRing& ring, const std::vector<STileOut>& laid, float at);

    // the yaw of a point, seen from the ring's center
    float ringYaw(const STileRing& ring, const V3& p);

    // where a view's middle crosses the ring going out, from `eye` turned `yaw` and pitched `pitch` (up +): on the ring's
    // cylinder, `radius` out round its center, where its windows' middles are. Pitched more than 45 degrees up or down,
    // as at 45 (a window higher or lower round you would be seen too much from below or above). False when it doesn't
    // cross it (from outside it, looking away)
    bool ringLook(const STileRing& ring, const V3& eye, float yaw, float pitch, V3& at);

    // whether a point is at the ring: no further from its center than its windows stand (a ring that stays where it is,
    // Y: what you open, bring or put down there goes into the row; away from it, it doesn't)
    bool insideRing(const STileRing& ring, const V3& p);

    // What the ring stands on as it goes with you (the plugin moves it with you every frame, walking, running and
    // flying): the height your feet are seen at, up and down stairs and slopes with you, but not up with a jump (down
    // with a fall, though), up onto what you land on a moment after you (at RISE a second), up and down with you
    // flying; where you're put (further than PUT in one step: the spawn, a teleport), at once
    struct STileFloor {
        static constexpr float RISE = 10.f, PUT = 2.f;
        float                  y      = 0;     // what it stands on now
        float                  to     = 0;     // where that goes: where you stand, or where you jumped from
        V3                     was;            // your feet the step before
        bool                   ground = false; // ... on the ground then

        void                   reset(const V3& feet);
        void                   step(const V3& feet, bool onGround, bool fly, float dt);
    };
}
