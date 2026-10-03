#include "tiling.hpp"

#include <numbers>

namespace h3d {

    namespace {
        constexpr float PI       = std::numbers::pi_v<float>;
        constexpr float LOOK_TAN = 1.f; // ringLook: the most a view's pitch goes up or down, as its tangent (45 degrees)

        float wrap(float a) {
            a = std::fmod(a + PI, 2.f * PI);
            if (a < 0)
                a += 2.f * PI;
            return a - PI;
        }

        // how far up or down from its middle a window may reach: `fit` of the view's half height, where the view sees
        // it when you face it (back + radius out)
        float reach(const STileRing& ring) {
            return ring.fit * (ring.back + ring.radius) * ring.tanHalfFov;
        }

        // a window played in the row: one with its own fit and a size (one with none yet takes no room, as the others)
        bool played(const STileIn& w) {
            return w.fit > 0.f && w.w >= 1.f && w.h >= 1.f;
        }

        // how tall and wide the view is (metres) where the window played is, as the view sees it: lookDist out along it,
        // pitched lookPitch up or down, an upright window shows cos of its height there (and is cos as far again along the
        // view) and all of its width
        void playedView(const STileRing& ring, float& viewH, float& viewW) {
            const float d = std::isnan(ring.lookDist) ? ring.back + ring.radius : ring.lookDist, c = std::cos(std::clamp(ring.lookPitch, -1.2f, 1.2f));
            viewH = 2.f * d * ring.tanHalfFov / (c * c);
            viewW = 2.f * d * ring.tanHalfFov * ring.aspect / c;
        }

        // metres per px, before the row is made to fit round you: as big as on your screen, made smaller to fit `fit` of
        // the view's height and width, and short enough to stand on the ground within the view's reach; the one played,
        // just its own fit of the view's height or width, whichever it fills first (bigger than on your screen, too), the
        // view as you see it
        float fullScale(const STileRing& ring, const STileIn& w) {
            const float ahead = ring.back + ring.radius;
            float       viewH = 2.f * ahead * ring.tanHalfFov, viewW = viewH * ring.aspect;
            if (played(w)) {
                playedView(ring, viewH, viewW);
                return std::min(w.fit * viewH / w.h, w.fit * viewW / w.w);
            }
            const float h = std::max(w.h, 1.f), wide = std::max(w.w, 1.f);
            float       s = std::min({viewH / ring.monitorH, ring.fit * viewH / h, ring.fit * viewW / wide});
            const float r = reach(ring), top = ring.center.y + r, bottom = std::max(ring.center.y - r, ring.ground + 0.05f);
            if (top > bottom)
                s = std::min(s, (top - bottom) / h);
            return s;
        }
    }

    float ringYaw(const STileRing& ring, const V3& p) {
        return std::atan2(p.x - ring.center.x, -(p.z - ring.center.z));
    }

    bool insideRing(const STileRing& ring, const V3& p) {
        return length(p - ring.center) <= ring.radius;
    }

    bool ringLook(const STileRing& ring, const V3& eye, float yaw, float pitch, V3& at) {
        const V3    d{std::sin(yaw), 0, -std::cos(yaw)};
        const float ox = eye.x - ring.center.x, oz = eye.z - ring.center.z, b = ox * d.x + oz * d.z, c = ox * ox + oz * oz - ring.radius * ring.radius;
        const float disc = b * b - c;
        if (!(disc >= 0.f))
            return false;
        const float s = -b + std::sqrt(disc); // (how far out, level: the far crossing, from inside the ring the one ahead)
        if (!(s > 0.f))
            return false;
        at   = eye + d * s;
        at.y = eye.y + s * std::clamp(std::tan(pitch), -LOOK_TAN, LOOK_TAN);
        return true;
    }

    std::vector<STileOut> layoutRing(const STileRing& ring, const std::vector<STileIn>& windows) {
        const size_t          n = windows.size();
        std::vector<STileOut> out(n);
        if (n == 0)
            return out;
        const float        R = std::max(ring.radius, 0.1f);
        std::vector<float> full(n);
        for (size_t i = 0; i < n; ++i)
            full[i] = fullScale(ring, windows[i]);
        // how far round the row goes with every window f times as big, the one played g times (a flat window w wide, R
        // out, takes 2 atan(w / 2R))
        const auto span = [&](float f, float g) {
            float a = ring.gap * (float)(n - 1);
            for (size_t i = 0; i < n; ++i)
                a += 2.f * std::atan(std::max(windows[i].w, 0.f) * full[i] * (played(windows[i]) ? g : f) * 0.5f / R);
            return a;
        };
        // too far round: the others all smaller alike, the one played as it is; only if even with them at nothing it
        // would be (a hundred windows or so), it's made smaller too
        float f = 1.f, g = 1.f;
        if (span(1.f, 1.f) > ring.most) {
            const bool others = span(0.f, 1.f) <= ring.most;
            float      lo = 0.f, hi = 1.f;
            for (int it = 0; it < 40; ++it) {
                const float mid = 0.5f * (lo + hi);
                ((others ? span(mid, 1.f) : span(0.f, mid)) > ring.most ? hi : lo) = mid;
            }
            f = others ? lo : 0.f;
            g = others ? 1.f : lo;
        }
        const float r  = reach(ring);
        float       at = ring.yaw - 0.5f * span(f, g); // the row's left end
        for (size_t i = 0; i < n; ++i) {
            STileOut& o = out[i];
            o.scale     = full[i] * (played(windows[i]) ? g : f);
            o.half      = std::atan(std::max(windows[i].w, 0.f) * o.scale * 0.5f / R);
            o.angle     = wrap(at + o.half);
            at += 2.f * o.half + ring.gap;
            const V3 dir{std::sin(o.angle), 0, -std::cos(o.angle)};
            o.normal = -dir;
            o.up     = {0, 1, 0};
            o.right  = cross(o.up, o.normal);
            o.center = ring.center + dir * R;
            // level with the center, or standing on the ground; the one played where you look (not on the ground: it's
            // drawn over the world while it's played)
            const float hh = std::max(windows[i].h, 0.f) * o.scale * 0.5f;
            if (played(windows[i]))
                o.center.y = std::isnan(ring.lookY) ? ring.center.y : ring.lookY;
            else
                o.center.y = std::max(std::min(ring.center.y, ring.center.y + r - hh), ring.ground + 0.05f + hh);
        }
        return out;
    }

    float viewShare(const STileRing& ring, const STileIn& w, float scale) {
        float viewH, viewW;
        playedView(ring, viewH, viewW);
        return std::max(w.h * scale / viewH, w.w * scale / viewW);
    }

    void STileFloor::reset(const V3& feet) {
        y = to = feet.y;
        was    = feet;
        ground = false;
    }

    void STileFloor::step(const V3& feet, bool onGround, bool fly, float dt) {
        const float before  = to;
        const bool  walking = ground && onGround && !fly; // (on the ground since the step before)
        to                  = fly || onGround ? feet.y : std::min(to, feet.y);
        if (!(length(feet - was) <= PUT))
            y = to = feet.y;
        else if (fly || to < y)
            y = to;
        else {
            if (walking) // (up stairs and slopes with you, what's left of a landing going on)
                y += to - before;
            y += (to - y) * (1.f - std::exp(-dt * RISE));
        }
        was    = feet;
        ground = onGround && !fly;
    }

    int ringSlot(const STileRing& ring, const std::vector<STileOut>& laid, float at) {
        const float rel = wrap(at - ring.yaw);
        int         k   = 0;
        for (const auto& o : laid)
            if (wrap(o.angle - ring.yaw) < rel + 1e-4f) // (a window whose middle is right there, rounded either side of it: after it)
                ++k;
        return k;
    }
}
