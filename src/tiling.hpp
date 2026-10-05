#pragma once

#include "math3d.hpp"

#include <vector>

namespace hyprwalk {

    // Tiling mode's ring (T): windows side by side round the view, upright and facing its center, each as big as on the
    // monitor but shrunk to fit the view. The row is centred where you looked and shrinks evenly past `most`; a window
    // played in it (STileIn::fit) keeps its share of the view and the others shrink instead
    struct STileRing {
        V3    center;               // eye (third person: the avatar's head)
        float yaw        = 0;       // row's middle; 0 = -z, turning right is +
        float radius     = 2.f;     // metres from the center
        float back       = 0;       // view distance behind the center (camera boom)
        float ground     = -1e9f;   // floor height; windows stay 5 cm above it
        float fit        = 0.85f;   // max share of view height/width per window
        float tanHalfFov = 0.7f;    // tan of half the vertical fov
        float aspect     = 16.f / 9; // view width / height
        float monitorH   = 1440;    // logical px; a window this tall fills the view
        float gap        = 0.04f;   // radians between two windows
        float most       = 5.93f;   // max angle the row spans, radians
        float lookY      = NAN;     // played window's middle height (ringLook); NaN: center
        float lookDist   = NAN;     // level distance from the view to it; NaN: back + radius
        float lookPitch  = 0;       // view pitch to it; its fit is measured from there
    };

    struct STileIn {
        float w = 0, h = 0; // logical px
        float fit = 0;      // played window's view share (play_size); 0 = normal
    };

    struct STileOut {
        V3    center;            // its middle
        V3    right, up, normal; // normal is level, towards the ring's center
        float scale = 0;         // metres per logical px
        float angle = 0;         // yaw of its middle from the ring's center
        float half  = 0;         // half its angular width
    };

    // the row, left to right
    std::vector<STileOut> layoutRing(const STileRing& ring, const std::vector<STileIn>& windows);

    // view share of a window at `scale` m/px, seen like the played window: the larger of height and width
    float viewShare(const STileRing& ring, const STileIn& w, float scale);

    // insertion index for something at yaw `at`: after every window whose middle is left of it or right at it (so one
    // opening where a played window is goes after it); 0 .. n
    int ringSlot(const STileRing& ring, const std::vector<STileOut>& laid, float at);

    // the yaw of a point, seen from the ring's center
    float ringYaw(const STileRing& ring, const V3& p);

    // where the view from `eye` (yaw, pitch up +) crosses the ring's cylinder going out; pitch is clamped to ±45° so
    // windows aren't seen too much from below or above. False when it misses (outside the ring, looking away)
    bool ringLook(const STileRing& ring, const V3& eye, float yaw, float pitch, V3& at);

    // inside the ring: with a staying ring (Y), windows opened, brought or put down here join the row
    bool insideRing(const STileRing& ring, const V3& p);

    // floor of a ring that follows you: tracks your feet on stairs, slopes and in flight, ignores jumps (not falls),
    // rises onto a landing at RISE a second, and snaps when you move more than PUT in a step (spawn, teleport)
    struct STileFloor {
        static constexpr float RISE = 10.f, PUT = 2.f;
        float                  y      = 0;     // current floor height
        float                  to     = 0;     // target: your feet, or where you jumped from
        V3                     was;            // your feet the step before
        bool                   ground = false; // and whether on the ground then

        void                   reset(const V3& feet);
        void                   step(const V3& feet, bool onGround, bool fly, float dt);
    };
}
