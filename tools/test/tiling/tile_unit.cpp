// Tiling mode's ring (src/tiling.cpp) without Hyprland, and the floor it follows with the plugin's body (walker.cpp).
// run.sh builds and runs it.
#include "tiling.hpp"
#include "walker.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <numbers>
#include <string>

using namespace h3d;

namespace {
    int  g_passed = 0, g_failed = 0;

    void check(bool ok, const std::string& what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        ++(ok ? g_passed : g_failed);
    }

    constexpr float PI = std::numbers::pi_v<float>;

    bool near(float a, float b, float eps) {
        return std::abs(a - b) <= eps;
    }

    float unwrap(float a, float around) { // a, within pi of `around`
        while (a - around > PI)
            a -= 2.f * PI;
        while (a - around < -PI)
            a += 2.f * PI;
        return a;
    }

    STileRing firstPerson() {
        STileRing r;
        r.center     = {10, 1.65f, -4};
        r.yaw        = 0.3f;
        r.radius     = 2.f;
        r.fit        = 0.85f;
        r.tanHalfFov = std::tan(35.f * PI / 180.f);
        r.aspect     = 16.f / 9.f;
        r.monitorH   = 1440;
        return r;
    }

    // the row: left to right, `gap` apart, centred on the ring's yaw, radius out, facing the center
    void rowChecks(const std::string& name, const STileRing& ring, const std::vector<STileIn>& in, const std::vector<STileOut>& out) {
        check(out.size() == in.size(), std::format("{}: a slot for each of the {} windows", name, in.size()));
        if (out.size() != in.size() || out.empty())
            return;
        bool  spaced = true, round = true, facing = true, handed = true, widths = true;
        float prev   = unwrap(out[0].angle, ring.yaw);
        for (size_t i = 0; i < out.size(); ++i) {
            const STileOut& o = out[i];
            const float     a = unwrap(o.angle, prev);
            if (i > 0 && !near((a - o.half) - (prev + out[i - 1].half), ring.gap, 2e-4f))
                spaced = false;
            prev             = a;
            const V3    flat = V3{o.center.x - ring.center.x, 0, o.center.z - ring.center.z};
            if (!near(length(flat), ring.radius, 1e-3f))
                round = false;
            if (dot(o.normal, normalize(-flat)) < 0.9999f || !near(o.normal.y, 0, 1e-6f))
                facing = false;
            if (dot(o.right, V3{std::cos(o.angle), 0, std::sin(o.angle)}) < 0.9999f)
                handed = false;
            if (!near(2.f * o.half, 2.f * std::atan(in[i].w * o.scale * 0.5f / ring.radius), 1e-4f))
                widths = false;
        }
        const float left  = unwrap(out.front().angle, ring.yaw) - out.front().half;
        const float right = unwrap(out.back().angle, ring.yaw) + out.back().half;
        check(spaced, std::format("{}: left to right, {:.3f} rad apart", name, ring.gap));
        check(near(0.5f * (left + right), ring.yaw, 2e-4f), std::format("{}: centred on where you looked ({:.4f} vs {:.4f})", name, 0.5f * (left + right), ring.yaw));
        check(round, std::format("{}: {} m out from the center", name, ring.radius));
        check(facing, name + ": each faces the center, upright");
        check(handed, name + ": each reads left to right as you face it");
        check(widths, name + ": each takes the angle its width does");
        check(right - left <= ring.most + 1e-4f, std::format("{}: {:.2f} rad round you, at most {:.2f}", name, right - left, ring.most));
        for (size_t i = 0; i < out.size(); ++i)
            if (!near(ringYaw(ring, out[i].center), out[i].angle, 1e-4f)) {
                check(false, std::format("{}: ringYaw gives window {}'s yaw back", name, i));
                return;
            }
        check(true, name + ": ringYaw gives each window's yaw back");
    }
}

int main() {
    // three windows on a 2560x1440 monitor: two half its height, one the whole of it
    {
        const STileRing            ring = firstPerson();
        const std::vector<STileIn> in   = {{1230, 650}, {1230, 650}, {1230, 1350}};
        const auto                 out  = layoutRing(ring, in);
        rowChecks("three windows, first person", ring, in, out);
        const float viewH = 2.f * ring.radius * ring.tanHalfFov;
        check(near(out[0].scale, viewH / 1440.f, 1e-7f) && near(out[1].scale, viewH / 1440.f, 1e-7f),
              std::format("the short ones as big as on your screen ({:.6f} m/px)", out[0].scale));
        check(near(out[2].scale, 0.85f * viewH / 1350.f, 1e-7f), std::format("the tall one made smaller to fit 85% of the view ({:.6f} m/px)", out[2].scale));
        check(near(out[0].center.y, 1.65f, 1e-5f) && near(out[2].center.y, 1.65f, 1e-5f), "level with your eye");
        const float span = unwrap(out[2].angle, ring.yaw) + out[2].half - (unwrap(out[0].angle, ring.yaw) - out[0].half);
        check(span > 2.f && span < 3.5f, std::format("about half way round you ({:.0f} degrees)", span * 180.f / PI));
    }
    // one window: in front of you, where you looked
    {
        const STileRing ring = firstPerson();
        const auto      out  = layoutRing(ring, {{2540, 1350}});
        check(out.size() == 1 && near(out[0].angle, ring.yaw, 1e-5f), "one window: straight ahead");
        check(near(out[0].scale, 0.85f * 2.f * ring.radius * ring.tanHalfFov * ring.aspect / 2540.f, 1e-7f) ||
                  near(out[0].scale, 0.85f * 2.f * ring.radius * ring.tanHalfFov / 1350.f, 1e-7f),
              "one window the whole monitor wide: fits 85% of the view");
    }
    // many windows: all smaller alike, the row going no further round than `most`
    {
        const STileRing      ring = firstPerson();
        std::vector<STileIn> in(16, STileIn{1920, 1080});
        const auto           out = layoutRing(ring, in);
        rowChecks("sixteen windows", ring, in, out);
        const float full = std::min(2.f * ring.radius * ring.tanHalfFov / 1440.f, 0.85f * 2.f * ring.radius * ring.tanHalfFov / 1080.f);
        check(out[0].scale < full * 0.6f, std::format("sixteen windows: made smaller ({:.2f} of their size alone)", out[0].scale / full));
        bool alike = true;
        for (const auto& o : out)
            alike = alike && near(o.scale, out[0].scale, 1e-9f);
        check(alike, "sixteen windows: all alike");
    }
    // third person: past the avatar, seen from the camera behind it; a tall window stands on the ground
    {
        STileRing ring = firstPerson();
        ring.center    = {0, 1.55f, 0};
        ring.yaw       = -2.5f; // the row crosses -pi
        ring.back      = 2.6f;
        ring.radius    = 3.6f;
        ring.fit       = 0.6f;
        ring.ground    = 0.f;
        const std::vector<STileIn> in = {{1000, 1400}, {800, 600}, {1920, 1080}, {640, 480}};
        const auto                 out = layoutRing(ring, in);
        rowChecks("third person", ring, in, out);
        const float reach = 0.6f * (2.6f + 3.6f) * ring.tanHalfFov;
        bool        onGround = true, inView = true;
        for (size_t i = 0; i < out.size(); ++i) {
            const float hh = in[i].h * out[i].scale * 0.5f;
            onGround       = onGround && out[i].center.y - hh >= 0.05f - 1e-4f;
            inView         = inView && out[i].center.y + hh <= ring.center.y + reach + 1e-4f;
        }
        check(onGround, "third person: every window's bottom 5 cm or more above the ground");
        check(inView, "third person: none reaches higher than the view does");
        check(out[0].center.y - in[0].h * out[0].scale * 0.5f < 0.06f, "third person: the tall one stands on the ground");
    }
    // ringSlot: where a window carried to a yaw goes
    {
        const STileRing ring = firstPerson();
        const auto      out  = layoutRing(ring, {{1000, 800}, {1000, 800}, {1000, 800}});
        check(ringSlot(ring, out, out[1].angle - 0.01f) == 1, "at the middle window's left half: before it");
        check(ringSlot(ring, out, out[1].angle + 0.01f) == 2, "at its right half: after it");
        check(ringSlot(ring, out, ring.yaw + PI - 0.01f) == 3, "behind you, from the right: at the right end");
        check(ringSlot(ring, out, ring.yaw - PI + 0.01f) == 0, "behind you, from the left: at the left end");
        check(ringSlot(ring, {}, 1.f) == 0, "an empty row: the first");
        check(ringSlot(ring, out, out[1].angle) == 2, "right at its middle: after it");
    }
    // a window opening where you look goes after the played window the row is turned to, whatever the rounding
    {
        STileRing            ring = firstPerson();
        std::vector<STileIn> in   = {{280, 240}, {280, 240}, {1200, 750}, {280, 240}, {280, 240}};
        in[2].fit                 = 0.5f;
        int      after = 0, n = 0;
        uint32_t seed  = 12345;
        const auto next = [&] { // 0..1
            seed = seed * 1664525u + 1013904223u;
            return (float)(seed >> 8) / 16777216.f;
        };
        for (; n < 2000; ++n) {
            ring.yaw         = (next() * 2.f - 1.f) * PI;
            const float look = (next() * 2.f - 1.f) * PI;
            ring.yaw         = std::remainder(ring.yaw + std::remainder(look - layoutRing(ring, in)[2].angle, 2.f * PI), 2.f * PI);
            after += ringSlot(ring, layoutRing(ring, in), look) == 3;
        }
        check(after == n, std::format("a window played turned to where you look: one opening there goes after it ({} of {} times)", after, n));
    }
    // insideRing: within the radius of the ring's middle, up and down too
    {
        const STileRing ring = firstPerson();
        const V3        c    = ring.center;
        check(insideRing(ring, c), "at the ring's middle: at the ring");
        check(insideRing(ring, c + V3{1.2f, 0, -1.5f}), "walked up to a window, 1.9 m out: at the ring");
        check(insideRing(ring, c + V3{0, 0, 2.f}) && insideRing(ring, c + V3{-2.f, 0, 0}), "where its windows stand: still at it");
        check(!insideRing(ring, c + V3{0, 0, 2.05f}) && !insideRing(ring, c + V3{1.5f, 0, 1.5f}), "just past its windows, and 2.1 m out between two: away from it");
        check(!insideRing(ring, c + V3{0, 2.5f, 0}), "flown 2.5 m up over its middle: away from it");
        check(!insideRing(ring, c + V3{-25.f, 0, 30.f}), "streets away: away from it");
        STileRing third = ring;
        third.radius    = 3.6f;
        check(insideRing(third, c + V3{0, 0, 3.f}) && !insideRing(third, c + V3{0, 0, 3.7f}), "third person's wider ring: at it 3 m out, away 3.7 m out");
    }
    // a window not sized yet takes no room, and nothing is NaN
    {
        const STileRing ring = firstPerson();
        const auto      out  = layoutRing(ring, {{0, 0}, {1200, 900}});
        check(out.size() == 2 && out[0].half == 0.f && std::isfinite(out[0].center.x + out[0].center.y + out[1].center.z + out[1].scale),
              "a window with no size yet: no room, no NaN");
        check(layoutRing(ring, {}).empty(), "no windows: nothing");
    }
    // a played window (play_size) takes that share of the view's height or width, whichever it fills first; the others
    // stay as they were
    {
        const STileRing            ring  = firstPerson();
        const float                viewH = 2.f * ring.radius * ring.tanHalfFov, viewW = viewH * ring.aspect;
        const std::vector<STileIn> alike = {{800, 600}, {2540, 1000}, {1000, 1400}, {800, 600}};
        std::vector<STileIn>       in    = alike;
        in[1].fit                        = 0.5f;
        const auto a = layoutRing(ring, alike), b = layoutRing(ring, in);
        rowChecks("a wide window played in the row", ring, in, b);
        check(near(a[1].scale * 2540.f, 0.85f * viewW, 1e-4f), std::format("a wide window, not played: 85% of the view's width ({:.3f})", a[1].scale * 2540.f / viewW));
        check(near(b[1].scale * 2540.f, 0.5f * viewW, 1e-4f), std::format("... played with fit 0.5: half of it ({:.3f})", b[1].scale * 2540.f / viewW));
        bool same = true;
        for (const size_t i : {0, 2, 3})
            same = same && near(b[i].scale, a[i].scale, 1e-9f) && near(b[i].center.y, a[i].center.y, 1e-6f);
        check(same, "... the others as big as they were, where they were up and down");
        check(near(b[1].center.y, ring.center.y, 1e-5f), "... no lookY: level with your eye");
        const float spanA = unwrap(a.back().angle, ring.yaw) + a.back().half - (unwrap(a.front().angle, ring.yaw) - a.front().half);
        const float spanB = unwrap(b.back().angle, ring.yaw) + b.back().half - (unwrap(b.front().angle, ring.yaw) - b.front().half);
        check(spanB < spanA - 0.1f, std::format("... the row closes up round it ({:.0f} degrees round you, {:.0f} before)", spanB * 180.f / PI, spanA * 180.f / PI));
        // a tall one, then small ones made bigger
        in[1].fit = 0.f;
        in[2].fit = 0.5f;
        check(near(layoutRing(ring, in)[2].scale * 1400.f, 0.5f * viewH, 1e-4f), "a tall window played with fit 0.5: half the view's height");
        in[2].fit = 0.94f;
        check(near(layoutRing(ring, in)[2].scale * 1400.f, 0.94f * viewH, 1e-4f), "... with fit 0.94: 94% of it (more than the ring's 0.85: the one played is as big as it's given)");
        in[2].fit = 0.f;
        in[0].fit = 0.45f;
        check(near(layoutRing(ring, in)[0].scale * 600.f, 0.45f * viewH, 1e-4f) && layoutRing(ring, in)[0].scale > viewH / 1440.f * 1.07f,
              "a small one (42% of the view's height on your screen) with fit 0.45: made bigger, 45% of it");
        in[0].fit = 0.25f;
        check(near(layoutRing(ring, in)[0].scale, 0.25f * viewH / 600.f, 1e-9f), "... with fit 0.25: a quarter of the view's height");
        std::vector<STileIn> small = {{280, 240}, {280, 240, 0.5f}, {280, 240}};
        const auto           s     = layoutRing(ring, small);
        check(near(s[1].scale * 240.f, 0.5f * viewH, 1e-4f) && near(s[0].scale, viewH / 1440.f, 1e-9f) && s[1].scale > 2.9f * s[0].scale,
              std::format("a small game (280x240, a sixth of the view's height on your screen) played with fit 0.5: made bigger, half of it ({:.1f} times as big as its "
                          "neighbours, as on your screen)", s[1].scale / s[0].scale));
    }
    // played at 0.25, 0.5 and 0.94, first and third person, the ground 0.2 m below the ring's middle: its share of the
    // view, at the ring's middle or lookY, off the ground; the big ones beside it stand on it
    for (const bool third : {false, true}) {
        STileRing ring = firstPerson();
        if (third) {
            ring.center = {0, 1.55f, 0};
            ring.yaw    = -2.5f;
            ring.back   = 2.6f;
            ring.radius = 3.6f;
            ring.fit    = 0.6f;
        }
        ring.ground       = ring.center.y - 0.2f;
        const float ahead = ring.back + ring.radius, viewH = 2.f * ahead * ring.tanHalfFov, viewW = viewH * ring.aspect, reach = ring.fit * ahead * ring.tanHalfFov;
        const std::string who = third ? "third person" : "first person";
        for (const float fit : {0.25f, 0.5f, 0.94f}) {
            bool        sized = true, level = true, looked = true, off = true, stand = true;
            std::string got;
            for (const bool wide : {true, false}) {
                const STileIn        game = wide ? STileIn{1920, 800, fit} : STileIn{1000, 1300, fit};
                std::vector<STileIn> in   = {{1600, 1000}, game, {1600, 1000}};
                STileRing            r    = ring;
                auto                 out  = layoutRing(r, in);
                if (wide && fit == 0.5f)
                    rowChecks(std::format("{}, the ground close under the middle, a game played among big windows", who), r, in, out);
                const float w = out[1].scale * game.w, h = out[1].scale * game.h, y = out[1].center.y;
                sized         = sized && (wide ? near(w, fit * viewW, 1e-4f) && h < fit * viewH : near(h, fit * viewH, 1e-4f) && w < fit * viewW);
                level         = level && near(y, ring.center.y, 1e-5f);
                off           = off && out[1].center.y - h * 0.5f < ring.ground + 0.05f - 1e-4f; // not standing
                for (const size_t i : {0, 2}) {
                    const float hh = in[i].h * out[i].scale * 0.5f;
                    stand          = stand && near(out[i].center.y - hh, ring.ground + 0.05f, 1e-4f) && out[i].center.y + hh <= ring.center.y + reach + 1e-4f;
                }
                r.lookY = ring.center.y + (wide ? 0.4f : -0.6f);
                out     = layoutRing(r, in);
                looked  = looked && near(out[1].center.y, r.lookY, 1e-6f) && near(out[1].scale * (wide ? game.w : game.h), fit * (wide ? viewW : viewH), 1e-4f);
                got += std::format("{}{} {:.3f} of the view's {}, its middle {:.2f} m (with lookY {:.2f}: {:.2f}), its bottom {:.2f}", got.empty() ? "" : "; ",
                                   wide ? "wide" : "tall", wide ? w / viewW : h / viewH, wide ? "width" : "height", y, r.lookY, out[1].center.y, y - h * 0.5f);
            }
            check(sized, std::format("{}, played with fit {:.2f}: just that much of the view's width (a wide game) or height (a tall one), the other less (the ground at "
                                     "{:.2f} m: {})", who, fit, ring.ground, got));
            check(level && looked, std::format("... {}, fit {:.2f}: its middle level with the ring's middle without lookY, and at lookY with one", who, fit));
            check(off && stand, std::format("... {}, fit {:.2f}: not standing on the ground close under it (as the big ones beside it do)", who, fit));
        }
    }
    // sized for the view as seen (lookDist, lookPitch): smaller with the boom pulled in by a wall, bigger by 1/cos² or
    // 1/cos when pitched
    {
        STileRing ring = firstPerson();
        ring.center    = {0, 1.55f, 0};
        ring.yaw       = -2.5f;
        ring.back      = 2.6f;
        ring.radius    = 3.6f;
        ring.fit       = 0.6f;
        for (const bool wide : {false, true}) {
            const STileIn              game = wide ? STileIn{1920, 800, 0.5f} : STileIn{1200, 750, 0.5f};
            const std::vector<STileIn> in   = {{1600, 1000}, game, {1600, 1000}};
            const auto                 nominal = layoutRing(ring, in);
            STileRing                  wall    = ring;
            wall.lookDist                      = 0.73f + 3.6f;
            const auto  pulled                 = layoutRing(wall, in);
            const float k                      = wall.lookDist / (ring.back + ring.radius);
            const char* what                   = wide ? "a wide game (1920x800)" : "a game (1200x750)";
            check(near(pulled[1].scale, nominal[1].scale * k, 1e-7f) && near(viewShare(wall, game, pulled[1].scale), 0.5f, 1e-5f) && near(pulled[0].scale, nominal[0].scale, 1e-9f),
                  std::format("third person, the camera's boom pulled in by a wall (0.73 m): {} played {:.3f} times as big, half of that view all the same ({:.4f}), the "
                              "others as the ring has them", what, pulled[1].scale / nominal[1].scale, viewShare(wall, game, pulled[1].scale)));
            for (const float deg : {25.f, -25.f}) {
                STileRing pitched = ring;
                pitched.lookPitch = deg * PI / 180.f;
                const auto  out   = layoutRing(pitched, in);
                const float c = std::cos(pitched.lookPitch), want = wide ? 1.f / c : 1.f / (c * c);
                check(near(out[1].scale, nominal[1].scale * want, 1e-7f) && near(viewShare(pitched, game, out[1].scale), 0.5f, 1e-5f),
                      std::format("... {} looked at {:+.0f} degrees: {:.3f} times as big (1/cos{} of it: its {} fills the view first), half the view as seen ({:.4f})", what, deg,
                                  out[1].scale / nominal[1].scale, wide ? "" : "²", wide ? "width" : "height", viewShare(pitched, game, out[1].scale)));
            }
        }
        const std::vector<STileIn> in = {{1600, 1000}, {1200, 750, 0.5f}, {1600, 1000}};
        check(near(viewShare(ring, in[1], layoutRing(ring, in)[1].scale), 0.5f, 1e-5f), "... and without lookDist, from back + radius, level: half the view");
    }
    // many windows, one played: the row stays within `most` and the others take all the squeeze
    {
        const STileRing      ring = firstPerson();
        std::vector<STileIn> in(16, STileIn{1920, 1080});
        const auto           alike = layoutRing(ring, in);
        in[7].fit                  = 0.5f;
        const auto out             = layoutRing(ring, in);
        rowChecks("sixteen windows, one played", ring, in, out);
        const float viewH = 2.f * ring.radius * ring.tanHalfFov, full = std::min(viewH / 1440.f, 0.85f * viewH / 1080.f), half = 0.5f * viewH * ring.aspect / 1920.f;
        bool        alikeOthers = true;
        for (size_t i = 0; i < out.size(); ++i)
            alikeOthers = alikeOthers && (i == 7 || near(out[i].scale, out[0].scale, 1e-9f));
        const float left = unwrap(out.front().angle, ring.yaw) - out.front().half, right = unwrap(out.back().angle, ring.yaw) + out.back().half;
        check(near(out[7].scale, half, 1e-9f) && near(right - left, ring.most, 1e-3f),
              std::format("sixteen windows, one played with fit 0.5: it takes just half the view ({:.4f}), the row round you as far as it goes ({:.3f} rad)", out[7].scale / half,
                          right - left));
        check(alikeOthers && out[0].scale < alike[0].scale * 0.95f && alike[7].scale < half,
              std::format("... the others all smaller alike, smaller than with none played ({:.2f} of their size alone, {:.2f} with none played, when it took {:.3f} of the "
                          "view too)", out[0].scale / full, alike[0].scale / full, alike[7].scale * 1920.f / (viewH * ring.aspect)));
        // eight: one played at 0.25 frees room that makes the others bigger
        std::vector<STileIn> eight(8, STileIn{1920, 1080});
        const auto           before = layoutRing(ring, eight);
        eight[3].fit                = 0.25f;
        const auto after            = layoutRing(ring, eight);
        rowChecks("eight windows, one played", ring, eight, after);
        check(near(after[3].scale, 0.25f * viewH * ring.aspect / 1920.f, 1e-9f) && before[3].scale > after[3].scale && after[0].scale > before[0].scale * 1.02f,
              std::format("eight windows, one played with fit 0.25: it takes a quarter of the view ({:.2f} squeezed with the others), and they're bigger for it "
                          "({:.3f} -> {:.3f} of the view's width)", before[3].scale * 1920.f / (viewH * ring.aspect), before[0].scale * 1920.f / (viewH * ring.aspect),
                          after[0].scale * 1920.f / (viewH * ring.aspect)));
    }
    // so many windows their gaps alone nearly fill the row: the others get nothing, the played one shrinks too
    {
        const STileRing      ring = firstPerson();
        std::vector<STileIn> in(110, STileIn{800, 600});
        in[55]         = {1920, 1080, 0.94f};
        const auto out = layoutRing(ring, in);
        rowChecks("110 windows, one played", ring, in, out);
        const float viewH = 2.f * ring.radius * ring.tanHalfFov, want = 0.94f * viewH / 1080.f;
        bool        none  = true;
        for (size_t i = 0; i < out.size(); ++i)
            none = none && (i == 55 || out[i].scale == 0.f);
        check(none && out[55].scale < want * 0.9f && out[55].scale > 0.f && std::isfinite(out[55].center.x + out[55].center.z),
              std::format("110 windows, one played with fit 0.94: the others at nothing, and it smaller too ({:.2f} of its share)", out[55].scale / want));
    }
    // ringLook: where the view's middle crosses the ring (lookY, and the yaw the row turns to)
    {
        STileRing ring = firstPerson();
        V3        at;
        const float t10 = std::tan(10.f * PI / 180.f);
        bool        ok  = ringLook(ring, ring.center, ring.yaw, 10.f * PI / 180.f, at);
        check(ok && near(ringYaw(ring, at), ring.yaw, 1e-5f) && near(length(V3{at.x - ring.center.x, 0, at.z - ring.center.z}), ring.radius, 1e-4f) &&
                  near(at.y, ring.center.y + ring.radius * t10, 1e-5f),
              std::format("from the ring's middle, 10 degrees up: straight out, {:.3f} m above your eye", at.y - ring.center.y));
        ok = ringLook(ring, ring.center, 0.7f, -0.3f, at);
        check(ok && near(ringYaw(ring, at), 0.7f, 1e-5f) && near(at.y, ring.center.y - ring.radius * std::tan(0.3f), 1e-5f), "... turned and looking down: out that way, below it");
        // third person: the camera 2.6 m behind the head (the ring's middle), 0.35 m right, 12 degrees down
        ring.center      = {0, 1.55f, 0};
        ring.yaw         = 0.4f;
        ring.radius      = 3.6f;
        const float yaw = 0.4f, pitch = -12.f * PI / 180.f;
        const V3    fwd{std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)}, right{std::cos(yaw), 0, std::sin(yaw)};
        const V3    eye = ring.center + right * 0.35f - fwd * 2.6f;
        ok              = ringLook(ring, eye, yaw, pitch, at);
        const V3    to  = at - eye;
        check(ok && near(length(V3{at.x, 0, at.z}), ring.radius, 1e-4f) && length(cross(normalize(to), fwd)) < 1e-4f && dot(to, fwd) > 0.f,
              std::format("third person, the camera behind the head looking 12 degrees down: on its view's middle, where that crosses the ring ({:.2f} m up)", at.y));
        // pitched far up: as at 45 degrees; outside the ring looking away: nowhere
        ok = ringLook(ring, ring.center, 0.f, 1.3f, at);
        check(ok && near(at.y, ring.center.y + ring.radius, 1e-4f), "looking 74 degrees up: as at 45 (as high above your eye as it's out)");
        check(!ringLook(ring, ring.center + V3{0, 0, 5.f}, PI, 0.f, at) && ringLook(ring, ring.center + V3{0, 0, 5.f}, 0.f, 0.f, at) && near(at.z, -ring.radius, 1e-4f),
              "outside it: looking away, nowhere; looking at it, where the view leaves it, across it");
    }

    // the ring's floor (STileFloor) following the plugin's body (walker.cpp, moved as simulate() does) over boxes: the
    // ground and a ledge 0.8 m up from x = 5, too high to step onto
    {
        constexpr float DT = 1.f / 144, GRAVITY = 20.f, JUMP = 6.3f, HEIGHT = 1.8f;
        CCollision      col;
        col.addBox({{-50, -1, -50}, {50, 0, 50}});
        col.addBox({{5, 0, -50}, {50, 0.8f, 50}});
        for (int i = 0; i < 10; ++i) // stairs from x = -2 toward -x, 17 cm steps, to 1.7 m
            col.addBox({{-2.f - 0.28f * (i + 1), 0, -50}, {-2.f - 0.28f * i, 0.17f * (i + 1), 50}});
        col.addBox({{-20, 0, -50}, {-4.8f, 1.7f, 50}});
        col.build();
        SWalker body;
        body.overlaps = [&](const V3& f, float h) {
            return col.overlaps({{f.x - SWalker::RADIUS, f.y, f.z - SWalker::RADIUS}, {f.x + SWalker::RADIUS, f.y + h, f.z + SWalker::RADIUS}});
        };
        body.feet = {0, 0, 0};
        STileFloor floor;
        floor.reset(body.seen());
        // simulates secs walking at vx (or flying at `fly`), jumping once if asked; returns the floor's range, how far
        // above the seen feet it got, and the feet's highest
        struct SSpan {
            float lo = 1e9f, hi = -1e9f, above = -1e9f, feetHi = -1e9f;
        };
        const auto run = [&](float secs, float vx, bool jump, const V3* fly = nullptr) {
            SSpan span;
            for (float t = 0; t < secs; t += DT) {
                if (fly)
                    body.vel = *fly;
                else {
                    body.vel.x = vx;
                    body.vel.z = 0;
                    body.vel.y = std::max(body.vel.y - GRAVITY * DT, -40.f);
                    if (jump && body.onGround) {
                        body.vel.y    = JUMP;
                        body.onGround = false;
                        jump          = false;
                    }
                }
                body.move(DT, HEIGHT, fly != nullptr);
                floor.step(body.seen(), body.onGround, fly != nullptr, DT);
                span.lo     = std::min(span.lo, floor.y);
                span.hi     = std::max(span.hi, floor.y);
                span.above  = std::max(span.above, floor.y - body.seen().y);
                span.feetHi = std::max(span.feetHi, body.feet.y);
            }
            return span;
        };
        SSpan s0 = run(1.f, 0, false);
        check(near(s0.lo, body.seen().y, 1e-3f) && near(s0.hi, body.seen().y, 1e-3f) && near(s0.hi, 0, 1e-3f), std::format("standing: the ring stands where you do ({:.5f} .. {:.5f}, you {:.5f})", s0.lo, s0.hi, body.seen().y));
        SSpan jump = run(0.55f, 0, true);
        check(jump.feetHi > 0.9f && jump.hi < 0.01f, std::format("a jump ({:.2f} m up): the ring stays down ({:.3f} m)", jump.feetHi, jump.hi));
        // landing, the seen body dips a little and recovers, the ring with it
        SSpan land = run(0.6f, 0, false);
        check(land.lo > -0.08f && near(floor.y, body.seen().y, 0.005f) && body.onGround,
              std::format("landing: the ring gives with you a little ({:.3f} m) and is back with you ({:.4f} m, you {:.4f})", land.lo, floor.y, body.seen().y));
        // onto the ledge: a running jump at it from 1.2 m before it
        body.feet  = {3.8f, 0, 0};
        body.seenY = NAN;
        run(0.2f, 0, false);
        SSpan up    = run(0.45f, 4.5f, true);
        SSpan onto  = run(0.02f, 0, false);
        const float right = floor.y;
        SSpan after = run(0.6f, 0, false);
        check(body.onGround && near(body.feet.y, 0.8f, 1e-3f) && up.hi < 0.01f, std::format("jumping onto a ledge: the ring stays down in the air ({:.3f} m)", up.hi));
        check(right < 0.6f && near(floor.y, 0.8f, 0.01f) && after.hi < 0.81f,
              std::format("... and comes up onto it a moment after you ({:.2f} m 3 frames after, {:.3f} m after 0.6 s, at most {:.3f})", right, floor.y, after.hi));
        (void)onto;
        // walking off the ledge (too high to step down): the ring follows, never above the seen feet
        SSpan off = run(1.5f, -1.6f, false);
        check(body.onGround && near(floor.y, body.seen().y, 0.005f) && near(body.feet.y, 0, 1e-3f) && off.above < 0.01f,
              std::format("walking off it: the ring goes down with you, never above you ({:.3f} m)", off.above));
        // running up the stairs and walking back down: with you all the way (not behind you going up)
        body.feet  = {-1.f, 0, 0};
        body.seenY = NAN;
        run(0.3f, 0, false);
        float lagUp = 0, lagDown = 0;
        for (int i = 0; i < 216; ++i) { // 1.5 s: 6 m, onto the top floor
            run(DT * 0.5f, -4.5f, false);
            lagUp = std::max(lagUp, std::abs(floor.y - body.seen().y));
        }
        const float upTop = body.feet.y;
        for (int i = 0; i < 576; ++i) { // 4 s: 6.3 m, past the bottom step
            run(DT * 0.5f, 1.6f, false);
            lagDown = std::max(lagDown, std::abs(floor.y - body.seen().y));
        }
        check(near(upTop, 1.7f, 1e-3f) && near(body.feet.y, 0, 1e-3f) && lagUp < 0.005f && lagDown < 0.005f,
              std::format("running up stairs and walking down them: the ring with you ({:.3f} m off going up, {:.3f} m down; up at {:.2f} m, down at {:.2f})", lagUp,
                          lagDown, upTop, body.feet.y));
        // flying: up and down with you, then out of the air falling down with you
        const V3 upward{0, 8, 0}, still{0, 0, 0};
        SSpan    fly = run(1.f, 0, false, &upward);
        check(near(floor.y, body.seen().y, 1e-5f) && floor.y > 7.f && fly.above < 1e-5f, std::format("flying up: the ring goes up with you ({:.2f} m)", floor.y));
        run(0.5f, 0, false, &still);
        body.vel      = {};
        body.onGround = false;
        SSpan fall    = run(1.5f, 0, false);
        check(body.onGround && near(floor.y, body.seen().y, 0.01f) && near(body.feet.y, 0, 1e-3f) && fall.above < 0.01f,
              std::format("out of the air: down with you as you fall ({:.3f} m above you at most)", fall.above));
        // at the plugin's longest step (50 ms): the ring stays down during a jump and settles with the seen body on
        // landing, a few cm up at most
        {
            SWalker slow = body;
            slow.feet    = {0, 0, 0};
            slow.vel     = {};
            slow.seenY   = NAN;
            STileFloor f;
            f.reset(slow.seen());
            float air = -1e9f, landed = -1e9f, top = 0;
            bool  jumped = false;
            for (int i = 0; i < 60; ++i) {
                slow.vel.y = std::max(slow.vel.y - GRAVITY * 0.05f, -40.f);
                if (i == 10 && slow.onGround) {
                    slow.vel.y    = JUMP;
                    slow.onGround = false;
                    jumped        = true;
                }
                slow.move(0.05f, HEIGHT, false);
                f.step(slow.seen(), slow.onGround, false, 0.05f);
                top = std::max(top, slow.feet.y);
                (jumped && !slow.onGround ? air : landed) = std::max(jumped && !slow.onGround ? air : landed, f.y);
            }
            check(top > 1.f && air < 1e-4f && landed < 0.07f && near(f.y, slow.seen().y, 0.005f),
                  std::format("a jump at 20 frames a second: the ring stays down in the air ({:.4f} m), settles with you landing ({:.3f} m at most)", air, landed));
        }
        // put somewhere (the spawn, hyprctl tp): there at once, even up
        body.feet  = {-30, 12, 20};
        body.seenY = NAN;
        floor.step(body.seen(), body.onGround, false, DT);
        check(near(floor.y, 12, 1e-5f), "put somewhere else: the ring's there at once");
    }

    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
