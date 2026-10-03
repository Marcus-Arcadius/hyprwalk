#include "walker.hpp"

#include <algorithm>
#include <cmath>

namespace h3d {

    void SWalker::moveAxis(int axis, float d, bool& blocked, float height, bool fly) {
        blocked = false;
        if (d == 0.f)
            return;

        // small steps so nothing is tunneled through
        const int   steps = std::max(1, (int)std::ceil(std::abs(d) / 0.1f));
        const float step  = d / steps;
        for (int i = 0; i < steps; ++i) {
            V3 p = feet;
            (&p.x)[axis] += step;
            if (!overlaps(p, height)) {
                feet = p;
                continue;
            }

            // walk up small ledges (stairs, the platform step)
            if (axis != 1 && onGround && !fly) {
                V3 up = p;
                up.y += STEP_HEIGHT;
                V3 upFrom = feet;
                upFrom.y += STEP_HEIGHT;
                if (!overlaps(upFrom, height) && !overlaps(up, height)) {
                    feet       = up;
                    bool       dummy     = false;
                    const bool wasGround = onGround;
                    moveAxis(1, -STEP_HEIGHT, dummy, height, fly); // settle on top of it
                    onGround = wasGround;
                    continue;
                }
            }

            // binary search the contact point for a snug stop
            float lo = 0.f, hi = 1.f;
            for (int k = 0; k < 8; ++k) {
                const float mid = (lo + hi) * 0.5f;
                V3          q   = feet;
                (&q.x)[axis] += step * mid;
                if (overlaps(q, height))
                    hi = mid;
                else
                    lo = mid;
            }
            (&feet.x)[axis] += step * lo;
            blocked = true;
            return;
        }
    }

    void SWalker::move(float dt, float height, bool fly) {
        const bool  wasGround = onGround && !fly;
        const float y0        = feet.y;
        bool        blocked   = false;
        moveAxis(0, vel.x * dt, blocked, height, fly);
        if (blocked)
            vel.x = 0;
        moveAxis(2, vel.z * dt, blocked, height, fly);
        if (blocked)
            vel.z = 0;
        const float vy = vel.y;
        moveAxis(1, vy * dt, blocked, height, fly);
        if (blocked) {
            onGround = vy < 0;
            vel.y    = 0;
        } else if (!fly) {
            onGround = false;
            // walking off a stair or down a slope, down onto it (else it falls off every step, the legs in the air, and
            // hops down a slope), as far as a ledge it would walk up
            if (wasGround && vy <= 0) {
                const V3 at   = feet;
                bool     down = false;
                moveAxis(1, -STEP_HEIGHT, down, height, fly);
                if (down)
                    onGround = true, vel.y = 0;
                else
                    feet = at;
            }
        }

        // as it's seen (exactly critically damped toward where it goes on at the climb's speed: as far from where it
        // was a frame ago as the climb takes it, where it is now)
        if (!(std::abs(feet.y - seenY) <= 1.f)) // (the first time, or it was put somewhere else)
            seenY = feet.y, seenV = climb = 0;
        const bool  walking = onGround && !fly;
        // (landing: the climb starts again from none, the fall's speed isn't a slope's, else what's seen goes on down
        // under the ground; and it stops with the body but for a little give, LAND_SEEN of how fast it came down)
        if (walking && !wasGround)
            climb = 0, seenV *= LAND_SEEN;
        const float rdt     = std::max(dt, 1e-4f);
        climb               = walking ? climb + ((feet.y - y0) / rdt - climb) * (1.f - std::exp(-dt * CLIMB_K)) : vel.y;
        const float lead = walking ? SEEN_LEAD * climb : climb, w = walking ? SEEN_W : SEEN_AIR_W, e = std::exp(-w * dt);
        const float x0 = seenY - (feet.y - lead * dt), v0 = seenV - lead, k = v0 + w * x0;
        seenY = feet.y + std::clamp((x0 + k * dt) * e, -STEP_HEIGHT, STEP_HEIGHT);
        seenV = lead + (v0 - w * k * dt) * e;
    }

    std::optional<float> groundUnder(const CCollision& col, float x, float z, float y) {
        if (SRayHit hit; col.raycast({x, y + 0.6f, z}, {0, -1, 0}, 1.4f, hit) && hit.normal.y > 0.5f)
            return y + 0.6f - hit.t;
        return std::nullopt;
    }
}
