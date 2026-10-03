// Offscreen test harness for hypr3d: drives the real renderer and avatar
// animator on a surfaceless EGL context and writes PNGs. No compositor needed.
//
// Arguments run in order, each changing the state; --out renders a picture:
//   shot --avatar a.vrm --frames 30 --view 0 --out front.png --view 180 --out back.png
#include "avatar.hpp"
#include "control.hpp"
#include "lipsync.hpp"
#include "menu.hpp"
#include "renderer.hpp"
#include "world.hpp"
#include "map.hpp"
#include "collision.hpp"
#include "walker.hpp"

#include <atomic>
#include <chrono>
#include <numeric>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace h3d {
    void log(const std::string& s) {
        fprintf(stderr, "[log] %s\n", s.c_str());
    }
    void notify(const std::string& s, bool error) {
        fprintf(stderr, "[notify%s] %s\n", error ? " error" : "", s.c_str());
    }
}

using namespace h3d;

namespace {
    // a load as the plugin's loader thread runs it (CBackgroundLoader): an exception makes it a failed load
    template <typename Res, typename Req>
    Res guardedLoad(Res (*fn)(const Req&, const std::atomic<bool>&), const Req& req) {
        std::atomic<bool> cancel = false;
        try {
            return fn(req, cancel);
        } catch (std::exception& e) {
            Res r;
            r.req   = req;
            r.error = std::format("loading {} failed: {}", req.path, e.what());
            return r;
        }
    }

    // ---- PNG, stored deflate (no zlib needed) ----
    uint32_t crcTable[256];
    void     initCrc() {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crcTable[n] = c;
        }
    }
    uint32_t crc(const uint8_t* d, size_t n, uint32_t c = 0xffffffffu) {
        for (size_t i = 0; i < n; ++i)
            c = crcTable[(c ^ d[i]) & 0xff] ^ (c >> 8);
        return c;
    }
    void be32(std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(x >> 24);
        v.push_back(x >> 16);
        v.push_back(x >> 8);
        v.push_back(x);
    }
    void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
        be32(out, data.size());
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        out.insert(out.end(), td.begin(), td.end());
        be32(out, crc(td.data(), td.size()) ^ 0xffffffffu);
    }
    bool writePNG(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
        std::vector<uint8_t> raw;
        raw.reserve((size_t)h * (w * 3 + 1));
        for (int y = 0; y < h; ++y) {
            raw.push_back(0);
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    raw.push_back(rgba[((size_t)y * w + x) * 4 + c]);
        }
        std::vector<uint8_t> z{0x78, 0x01};
        uint32_t             a = 1, b = 0;
        for (uint8_t v : raw) {
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
        for (size_t i = 0; i < raw.size(); i += 65535) {
            const size_t n = std::min<size_t>(65535, raw.size() - i);
            z.push_back(i + n == raw.size() ? 1 : 0);
            z.push_back(n & 0xff);
            z.push_back(n >> 8);
            z.push_back(~n & 0xff);
            z.push_back((~n >> 8) & 0xff);
            z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
        }
        be32(z, (b << 16) | a);

        std::vector<uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        std::vector<uint8_t> ihdr;
        be32(ihdr, w);
        be32(ihdr, h);
        ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
        chunk(out, "IHDR", ihdr);
        chunk(out, "IDAT", z);
        chunk(out, "IEND", {});
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)out.data(), out.size());
        return (bool)f;
    }

    // ---- EGL, surfaceless on a device (NVIDIA or Mesa) ----
    bool initEGL() {
        auto queryDevices = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
        auto platformDpy  = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        auto deviceString = (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
        if (!queryDevices || !platformDpy) {
            fprintf(stderr, "no EGL device enumeration\n");
            return false;
        }
        EGLDeviceEXT devs[8];
        EGLint       n = 0;
        queryDevices(8, devs, &n);
        const char* want = getenv("SHOT_DEVICE"); // substring of the DRM device file
        for (int i = 0; i < n; ++i) {
            const char* file = deviceString ? deviceString(devs[i], EGL_DRM_DEVICE_FILE_EXT) : nullptr;
            if (want && (!file || !strstr(file, want)))
                continue;
            EGLDisplay dpy = platformDpy(EGL_PLATFORM_DEVICE_EXT, devs[i], nullptr);
            EGLint     maj = 0, min = 0;
            if (!dpy || !eglInitialize(dpy, &maj, &min))
                continue;
            eglBindAPI(EGL_OPENGL_ES_API);
            const EGLint cfgAttr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
            EGLConfig    cfg;
            EGLint       ncfg = 0;
            if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &ncfg) || ncfg < 1)
                continue;
            const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
            EGLContext   ctx       = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
            if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
                continue;
            fprintf(stderr, "EGL device %s: %s\n", file ? file : "?", (const char*)glGetString(GL_RENDERER));
            return true;
        }
        fprintf(stderr, "no usable EGL device\n");
        return false;
    }

    V3 forwardFrom(float yaw, float pitch) {
        return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    }
    float rad(float deg) {
        return deg * 3.14159265f / 180.f;
    }
}

int main(int argc, char** argv) {
    initCrc();
    if (!initEGL())
        return 1;

    SWorld world = buildWorld(SScreenSpec{});
    CRenderer renderer;
    if (!renderer.init(world)) {
        fprintf(stderr, "renderer init failed\n");
        return 1;
    }

    std::shared_ptr<SAvatarModel> model;
    CAvatarAnimator               anim;
    CAvatarAnimator               still; // the same without physics, to measure the swing against
    V3                            move{}, vel{};
    float                         moveY = 0; // --movey: flying, the velocity up the keys ask for (the plugin's Space and Ctrl, or W looking up or down)
    float                         turn = 0;  // degrees a second
    float                         accel = 0; // m/s² toward the --move velocity, 0 = at once (the plugin's is 10 on the ground)
    float                         decel = 0; // m/s² when the --move velocity is slower than now (the plugin's is 14), 0 = accel
    float                         turnBack = 7; // m/s² when the --move velocity goes back the other way (the plugin's), 0 = accel
    bool                          face  = false; // --face 1: the body turns to where it goes, as the plugin's third person does
    bool                          lookCam = false; // --look-cam 1: the head looks as the plugin's third person has it, the camera behind looking -z
    float                         camLookYaw = 0, camLookPitch = 0; // (that, eased)
    float                         fpTurnYaw = 0, fpTurnPitch = 0; // --fpturn: the first person camera turning, degrees a second
    bool                          fpFollow = false, fpBodyTurning = false; // --fpfollow 1: the body turns as in the plugin's first person
    bool                          camWorld = false; // --view-world: the camera's yaw is the world's
    bool                          camChest = false; // --view-chest: ... the chest's (where it faces as the animation turns it)
    bool                          speedSet = false; // --speed: that speed along the body, whatever the velocity
    FILE*                         trace = nullptr;  // --trace: a line a frame, where the feet are
    FILE*                         bones = nullptr;  // --bones: a line a frame, where the humanoid's joints are
    FILE*                         clips = nullptr;  // --clip: a line a frame, how far the arms are inside the body and skirt
    std::vector<int8_t>           vertexPart;       // (--clip's: per vertex 0 the body, 1 the left forearm and hand, 2 the right's, -1 neither)
    FILE*                         hairClips = nullptr; // --hairclip: a line a frame, how far the hair is inside the arms
    std::vector<uint8_t>          vertexHair;          // (--hairclip's: per vertex, it hangs off the head on springs)
    FILE*                         springClips = nullptr; // --springclip: a line a frame, how far each kind of spring is inside the body
    FILE*                         springTrace = nullptr; // --springtrace: a line a frame, where the springs' tails are
    FILE*                         springDump  = nullptr; // --springdump: a line a frame, the springs' bones and colliders in the world
    FILE*                         bodyClips   = nullptr; // --bodyclip: a line a frame, how far each kind of spring is inside the body, bone by bone
    struct SBodyShape {                                  // (--bodyclip's: a bone's skin at rest round a line through it)
        int                joint = -1;
        V3                 o, u, e1, e2; // the line (bind space) and two ways across it
        float              a0   = 0;     // where its rows start along the line
        int                rows = 0;
        std::vector<float> out;          // rows * 72: how far out its skin goes, by 1 cm along the line and 5° round it
    };
    std::vector<SBodyShape>       bodyShapes;
    std::vector<int>              vertexBodyKind; // (per vertex: its kind of spring (bodyKinds), -1 the body, -2 neither)
    std::vector<std::string>      bodyKinds;
    std::string                   springPart;            // (... of the bones with that in their names)
    std::vector<int>              vertexSpring;          // (--springclip's: per vertex its kind of spring, -1 the body, -2 neither)
    std::vector<std::string>      springKinds;           // (... the kinds: a spring's name up to its first '.')
    float                         ground  = 0;
    bool                          jumping = false; // --jump: falls back down to ground
    bool                          walking = false; // --walk: the plugin's body through the world (walker.cpp): its stairs, ledges
    SWalker                       body;
    FILE*                         walkLog = nullptr; // --walklog: a line a frame, the body and the gait over the ground
    SAvatarMotion                 mo;
    std::vector<SPanel>           panels;
    int                           W = 800, H = 800;
    V3                            feet = world.spawn;
    V3 shift{};
    std::string aimName;
    float bodyYaw = 0, orbit = 0, pitch = 8, dist = 2.6f, targetY = -1, sky = 1, bounce = 0.15f, time = 0, fov = 70, exposure = 1;
    float height = 0;
    // first person (maps): from the feet at eye height, or from --eye
    bool  firstPerson = false, eyeSet = false;
    V3    eyeAt{};
    float camYaw = 0, camPitch = 0; // degrees, the plugin's: yaw 0 = -z, 90 = +x
    // first person with the body (--fpbody): the camera in the avatar's eyes, its head not drawn, its hands in view, as
    // the plugin has them; drawn from outside too (--third after it: the pose as it is)
    bool            fpBody = false;
    SFirstPersonEye fpEye;
    const float DEG = 180.f / 3.14159265f;
    // the plugin's adaptExposure(), snapped: how bright it is around the eye (the same functions)
    auto autoExposure = [&](const V3& eye, const V3& view) {
        if (!world.model)
            return 1.f;
        float samples[EXPOSURE_SAMPLES];
        for (int k = 0; k < EXPOSURE_SAMPLES; ++k)
            samples[k] = exposureSample(world, eye, exposureDirection(k));
        return exposureFor(world, samples, view);
    };
    bool autoExp = false;
    bool plainTextures = false; // --plain: no block compression (before --map)
    int  bench   = 0; // --bench n: time n more renders of each --out
    bool outlines = true; // --outlines 0: none of the avatar's toon outlines
    GLuint outTex = 0;
    int    texW = 0, texH = 0;

    // the Action Menu, as the plugin has it (view, fly and respawn only change what it shows)
    bool        third = true, fly = false;
    float       menuDt = 0.2f; // the time it's had at the next --out: 0.2 = all faded in, the flash gone
    // what the plugin's hyprctl, menu items and dials do to the avatar: the same code (control.cpp)
    CAvatarControl ctl(anim);
    CActionMenu    menu([&](const std::string& id) { return actionPage(id, {model.get(), &anim, false, third, fly}); },
                        [&](const SMenuItem& it, float v, float v2) { ctl.dial(it, v, v2); });
    float          wheel  = 0; // a fraction of a notch
    auto           menuDo = [&](const SMenuItem& it) {
        std::string r;
        switch (it.action) { // (main.cpp's own)
            case MA_VIEW:
                third = !third;
                r     = third ? "third" : "first";
                break;
            case MA_FLY:
                fly = !fly;
                r   = fly ? "flying" : "walking";
                break;
            case MA_RESPAWN: r = "ok"; break;
            default:
                r = ctl.action(it);
                if (r.empty())
                    r = "error: nothing to do";
        }
        fprintf(stderr, "menu picked %s (action %d, %d, %d): %s\n", it.label.c_str(), it.action, it.arg, it.arg2, r.c_str());
        return r;
    };
    const auto menuPick = [&](const std::optional<SMenuItem>& it) {
        if (it)
            menuDo(*it);
    };

    auto need = [&](int i, int k) {
        if (i + k >= argc) {
            fprintf(stderr, "%s needs %d value(s)\n", argv[i], k);
            exit(2);
        }
    };
    float frameDt = 1.f / 60;
    // lip sync from a WAV file (--audio), a frame's worth of it each frame, as the plugin does with the microphone
    CLipSync           lip;
    std::optional<float> lipGain; // --lipsync-gain (none: automatic)
    std::string           badgeText; // --badge
    float                 badgeScale = 1;
    std::vector<uint32_t> badgePixels;
    int                   badgeW = 0, badgeH = 0;
    uint64_t              badgeSerial = 0;
    std::vector<float> audio;
    int                audioRate = 0;
    size_t             audioAt   = 0;
    auto  step    = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            mo.dt = frameDt;
            time += mo.dt;
            if (!audio.empty() && audioAt < audio.size()) {
                const size_t n = std::min(audio.size() - audioAt, (size_t)std::lround(frameDt * audioRate));
                lip.feed(audio.data() + audioAt, n, audioRate);
                audioAt += n;
                anim.setVisemes(lip.visemes());
            } else if (!audio.empty()) {
                audio.clear();
                anim.setVisemes({});
            }
            if (mo.flying) {
                // (as the plugin's simulate() flying: toward what the keys ask for, up and down too, at 12 a second)
                vel = vel + (V3{move.x, moveY, move.z} - vel) * std::min(1.f, mo.dt * 12.f);
                mo.vy = vel.y;
            } else if (accel > 0) {
                V3          d  = V3{move.x, 0, move.z} - V3{vel.x, 0, vel.z};
                const bool  slower = decel > 0 && length(move) < length(V3{vel.x, 0, vel.z});
                const bool  back   = turnBack > 0 && dot(V3{move.x, 0, move.z}, V3{vel.x, 0, vel.z}) < 0;
                const float dl = length(d), maxD = (back ? turnBack : slower ? decel : accel) * mo.dt;
                if (dl > maxD)
                    d = d * (maxD / dl);
                vel.x += d.x;
                vel.z += d.z;
            } else {
                vel.x = move.x;
                vel.z = move.z;
            }
            if (!speedSet) {
                mo.speed = length(V3{vel.x, 0, vel.z});
                mo.vel   = {vel.x, 0, vel.z};
                mo.wish  = mo.flying ? V3{} : V3{move.x, 0, move.z}; // (flying, the plugin has no walk to ask for)
                if (accel > 0)
                    mo.accel = accel, mo.decel = decel > 0 ? decel : accel, mo.turnBack = turnBack > 0 ? turnBack : accel;
            }
            if (walking) {
                // (as the plugin's simulate(): its gravity, the air's 10 m/s² for the speed is left out)
                if (length(feet - body.seen()) > 1e-5f) // (put somewhere else: --pos)
                    body.feet = feet, body.seenY = NAN;
                body.vel.x = vel.x, body.vel.z = vel.z;
                if (mo.flying)
                    body.vel.y = vel.y;
                else
                    body.vel.y = std::max(body.vel.y - 20.f * mo.dt, -40.f);
                if (jumping && body.onGround && !mo.flying)
                    body.vel.y = 6.3f, body.onGround = false;
                jumping = false;
                body.move(mo.dt, 1.8f, mo.flying);
                vel         = body.vel;
                mo.onGround = body.onGround;
                mo.vy       = body.vel.y;
                mo.speed    = speedSet ? mo.speed : length(V3{vel.x, 0, vel.z});
                mo.vel      = {vel.x, 0, vel.z};
                feet        = body.seen(); // (the avatar, the camera: as the plugin draws them)
            } else if (jumping && !mo.flying) {
                vel.y -= 20.f * mo.dt;
                if (feet.y + vel.y * mo.dt <= ground) {
                    feet.y      = ground;
                    vel.y       = 0;
                    mo.onGround = true;
                    jumping     = false;
                }
                mo.vy = vel.y;
            }
            if (!walking) {
                // (flying: no lower than the ground it took off from; walking on it, nothing up or down)
                if (mo.flying && feet.y + vel.y * mo.dt < ground)
                    vel.y = std::max(vel.y, (ground - feet.y) / mo.dt);
                else if (!mo.flying && !jumping)
                    vel.y = 0;
                feet += vel * mo.dt;
            }
            if (face) // (as the plugin's third person: toward where it goes, else it stays; turning on from how fast it turns)
                if (const auto way = anim.wayToFace(mo, true, bodyYaw, 0.f)) // (W walks toward -z)
                    bodyYaw = anim.turnBody(bodyYaw, *way, mo.dt, mo.speed);
                else
                    bodyYaw = anim.turnBody(bodyYaw, mo.speed > 0.3f ? std::atan2(vel.x, -vel.z) : bodyYaw, mo.dt, mo.speed);
            else
                bodyYaw += rad(turn) * mo.dt;
            if (lookCam && model) {
                // (as the plugin's animateAvatar: where the camera looks, or back at it looking at its face; turning far,
                // where the body turns to; eased)
                auto      wrap = [](float a) { return std::remainder(a, 6.2831853f); };
                float     ly = wrap(0.f - bodyYaw), lp = 0.f;
                if (std::abs(ly) > 1.75f) {
                    const V3 d = feet + V3{0, 1.7f, 2.8f} - (feet + V3{0, model->height * 0.92f, 0});
                    ly         = wrap(std::atan2(d.x, -d.z) - bodyYaw);
                    lp         = std::atan2(d.y, std::hypot(d.x, d.z));
                }
                const float turning = smoothstep01((std::abs(anim.turnLeft()) - 0.3f) / 0.6f);
                ly *= 1.f - turning, lp *= 1.f - turning;
                const float k = 1.f - std::exp(-mo.dt * 6.f);
                camLookYaw += (std::clamp(ly, -1.4f, 1.4f) - camLookYaw) * k;
                camLookPitch += (std::clamp(lp, -1.1f, 1.1f) - camLookPitch) * k;
                mo.lookYaw = camLookYaw, mo.lookPitch = camLookPitch;
            }
            if (fpBody) { // (the mouse)
                camYaw += fpTurnYaw * mo.dt;
                camPitch = std::clamp(camPitch + fpTurnPitch * mo.dt, -89.f, 89.f);
            }
            if (fpBody && fpFollow && model) {
                // (as the plugin's animateAvatar in first person: toward where it goes, but no further off the camera than
                // 0.8 rad; standing, it catches up once the head would turn more than 0.9 rad)
                auto        wrap  = [](float a) { return std::remainder(a, 6.2831853f); };
                const float cy    = rad(camYaw);
                float       want  = bodyYaw;
                if (const auto way = anim.wayToFace(mo, false, bodyYaw, cy)) {
                    want          = *way;
                    fpBodyTurning = false;
                } else if (mo.speed > 0.3f) {
                    const float side = wrap(std::atan2(vel.x, -vel.z) - cy);
                    want             = std::abs(side) > 2.2f ? cy : cy + std::clamp(side, -0.8f, 0.8f);
                    fpBodyTurning    = false;
                } else {
                    const float off = wrap(cy - bodyYaw);
                    if (std::abs(off) > 0.9f)
                        fpBodyTurning = true;
                    if (fpBodyTurning) {
                        want = cy;
                        if (std::abs(off) < 0.05f)
                            fpBodyTurning = false;
                    }
                }
                bodyYaw = anim.turnBody(bodyYaw, want, mo.dt, mo.speed);
            }
            mo.fp.on = fpBody && model;
            if (mo.fp.on) {
                // (as the plugin's first person: the camera in the eyes as last drawn, the head looking where it looks)
                const auto e = anim.eyes();
                const M4   drawn = M4::trs(feet + V3{0, anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1});
                mo.fp.yaw        = rad(camYaw);
                mo.fp.pitch      = rad(camPitch);
                mo.fp.eye        = fpEye.update(feet, e ? drawn.point(*e) : feet + V3{0, 1.65f, 0}, mo.fp.yaw, mo.dt);
                mo.lookYaw       = std::remainder(mo.fp.yaw - bodyYaw, 6.2831853f);
                mo.lookPitch     = mo.fp.pitch;
            }
            mo.world = M4::trs(feet, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1});
            if (model) {
                anim.update(mo);
                still.update(mo);
            }
            mo.fp.press = false, mo.fp.tap = -1; // (once)
            if (springTrace && model) {
                // time, then each spring bone's tail (with the part in its name) in its parent's frame, as the springs
                // turned it (model units): how it swings on what it hangs from. First a line of what kind of spring
                // each is (its name up to its first '.', as --springclip has them)
                if (ftell(springTrace) == 0) {
                    std::string head = "# time";
                    for (const auto& J : model->springJoints)
                        if (model->nodes[J.node].name.find(springPart) != std::string::npos) {
                            const auto& n = model->springs[J.spring].name;
                            head += std::format(" [{}]", n.substr(0, n.find('.')));
                        }
                    fprintf(springTrace, "%s\n", head.c_str());
                }
                std::string line = std::format("{:.5f}", time);
                for (const auto& J : model->springJoints)
                    if (model->nodes[J.node].name.find(springPart) != std::string::npos) {
                        const int p = model->nodes[J.node].parent;
                        const V3  t = anim.globals()[J.node].point(J.tail), l = p >= 0 ? anim.globals()[p].inverse().point(t) : t;
                        line += std::format(" {:.6f} {:.6f} {:.6f}", l.x, l.y, l.z);
                    }
                fprintf(springTrace, "%s\n", line.c_str());
            }
            if (springDump && model) {
                // time, then each spring bone's joint and tail, then each collider's two ends, in the world (m). First a
                // line per bone (its name, spring, radius, length) and per collider (its node, kind, radius, the body's bit, a
                // disc's radius), and which colliders each spring keeps out of
                const M4 toWorld = M4::trs(feet + V3{0, anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1}) * model->fix;
                if (ftell(springDump) == 0) {
                    for (const auto& J : model->springJoints)
                        fprintf(springDump, "# joint %s %s %.4f %.4f\n", model->nodes[J.node].name.c_str(), model->springs[J.spring].name.c_str(), J.radius, J.length);
                    for (const auto& c : model->springColliders)
                        fprintf(springDump, "# collider %s %d %.4f %d %.4f\n", model->nodes[c.node].name.c_str(), (int)c.kind, c.radius, (int)c.body, c.disc);
                    for (const auto& s : model->springs) {
                        std::string ks;
                        for (int k : s.colliders)
                            ks += std::format(" {}", k);
                        fprintf(springDump, "# spring %s%s\n", s.name.c_str(), ks.c_str());
                    }
                }
                std::string line = std::format("{:.5f}", time);
                for (const auto& J : model->springJoints) {
                    const M4& g = anim.globals()[J.node];
                    for (const V3& p : {toWorld.point(g.point({})), toWorld.point(g.point(J.tail))})
                        line += std::format(" {:.5f} {:.5f} {:.5f}", p.x, p.y, p.z);
                }
                for (const auto& c : model->springColliders) {
                    const M4& g = anim.globals()[c.node];
                    for (const V3& p : {toWorld.point(g.point(c.offset)), toWorld.point(g.point(c.tail))})
                        line += std::format(" {:.5f} {:.5f} {:.5f}", p.x, p.y, p.z);
                }
                fprintf(springDump, "%s\n", line.c_str());
            }
            if (bodyClips && model) {
                // each bone's skin as it is at rest, round a line through the bone (toward the next bone of the humanoid,
                // or its first child, else its own longest way), by 1 cm along it and 5° round it; a spring's vertex
                // nearer that line than the skin goes out there, the bone as it is now, is inside the body. A bone's skin
                // is what it moves most of; the head's and what hangs from it, the hair, count as neither
                constexpr int   NB = 72;
                constexpr float DA = 0.01f;
                const auto&     J  = model->joints;
                if (vertexBodyKind.size() != model->vertices.size()) {
                    bodyKinds.clear();
                    bodyShapes.clear();
                    std::vector<int> kindOf(model->springs.size());
                    for (size_t s = 0; s < model->springs.size(); ++s) {
                        const std::string k  = model->springs[s].name.substr(0, model->springs[s].name.find('.'));
                        const auto        it = std::ranges::find(bodyKinds, k);
                        kindOf[s]            = (int)(it - bodyKinds.begin());
                        if (it == bodyKinds.end())
                            bodyKinds.push_back(k);
                    }
                    const size_t     nn = model->nodes.size();
                    std::vector<int> nodeKind(nn, -1), joint(nn, -1), jointOf(nn, -1);
                    for (const auto& jt : model->springJoints)
                        joint[jt.node] = kindOf[jt.spring];
                    for (size_t j = 0; j < J.size(); ++j)
                        jointOf[J[j].node] = (int)j;
                    const int nk = model->human[HB_NECK] >= 0 ? model->human[HB_NECK] : model->human[HB_HEAD];
                    for (size_t k = 0; k < nn; ++k) {
                        const int p = model->nodes[k].parent;
                        nodeKind[k] = joint[k] >= 0 ? joint[k] : (int)k == nk ? -2 : p >= 0 ? nodeKind[p] : -1;
                    }
                    std::vector<uint8_t> shown(model->vertices.size(), 0);
                    for (const auto& b : model->batches)
                        if (b.part < 0 || b.part >= (int)anim.partsShown().size() || anim.partsShown()[b.part])
                            for (uint32_t i = b.first; i < b.first + b.count; ++i)
                                shown[model->indices[i]] = 1;
                    vertexBodyKind.assign(model->vertices.size(), -2);
                    std::vector<std::vector<V3>> skin(J.size());
                    for (size_t v = 0; v < model->vertices.size(); ++v) {
                        const auto& vx   = model->vertices[v];
                        int         most = 0;
                        for (int k = 1; k < 4; ++k)
                            if (vx.weights[k] > vx.weights[most])
                                most = k;
                        if (!shown[v])
                            continue;
                        vertexBodyKind[v] = nodeKind[J[vx.joints[most]].node];
                        if (vertexBodyKind[v] == -1)
                            skin[vx.joints[most]].push_back({vx.pos[0], vx.pos[1], vx.pos[2]});
                    }
                    // the humanoid's next bones
                    std::vector<int> next(nn, -1);
                    auto             link = [&](int a, int b) {
                        if (model->human[a] >= 0 && model->human[b] >= 0)
                            next[model->human[a]] = model->human[b];
                    };
                    link(HB_HIPS, HB_SPINE);
                    link(HB_SPINE, HB_CHEST);
                    link(HB_CHEST, model->human[HB_UPPER_CHEST] >= 0 ? HB_UPPER_CHEST : HB_NECK);
                    link(HB_UPPER_CHEST, HB_NECK);
                    for (int s = 0; s < 2; ++s) {
                        link(s ? HB_R_SHOULDER : HB_L_SHOULDER, s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM);
                        link(s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM, s ? HB_R_LOWER_ARM : HB_L_LOWER_ARM);
                        link(s ? HB_R_LOWER_ARM : HB_L_LOWER_ARM, s ? HB_R_HAND : HB_L_HAND);
                        link(s ? HB_R_HAND : HB_L_HAND, fingerBone(s, FINGER_MIDDLE, 0));
                        link(s ? HB_R_UPPER_LEG : HB_L_UPPER_LEG, s ? HB_R_LOWER_LEG : HB_L_LOWER_LEG);
                        link(s ? HB_R_LOWER_LEG : HB_L_LOWER_LEG, s ? HB_R_FOOT : HB_L_FOOT);
                        link(s ? HB_R_FOOT : HB_L_FOOT, s ? HB_R_TOES : HB_L_TOES);
                        for (int f = 0; f < FINGER_COUNT; ++f)
                            for (int g = 0; g < 2; ++g)
                                link(fingerBone(s, f, g), fingerBone(s, f, g + 1));
                    }
                    for (size_t j = 0; j < J.size(); ++j) {
                        const auto& pts = skin[j];
                        if (pts.size() < 20)
                            continue;
                        SBodyShape sh;
                        sh.joint      = (int)j;
                        const M4 bind = J[j].inverseBind.inverse();
                        sh.o          = {bind.m[12], bind.m[13], bind.m[14]};
                        int to        = next[J[j].node];
                        if (to < 0)
                            for (size_t k = 0; k < nn && to < 0; ++k)
                                if (model->nodes[k].parent == J[j].node && jointOf[k] >= 0 && nodeKind[k] == -1)
                                    to = (int)k;
                        if (to >= 0 && jointOf[to] >= 0) {
                            const M4 b2 = J[jointOf[to]].inverseBind.inverse();
                            sh.u        = normalize(V3{b2.m[12], b2.m[13], b2.m[14]} - sh.o);
                        }
                        if (length(sh.u) < 0.5f) { // (its own longest way: the mic in a hand without fingers)
                            V3 mean{};
                            for (const V3& p : pts)
                                mean += p * (1.f / pts.size());
                            V3 d{1, 0.3f, 0.2f};
                            for (int it = 0; it < 30; ++it) {
                                V3 nd{};
                                for (const V3& p : pts)
                                    nd += (p - mean) * dot(p - mean, d);
                                d = normalize(nd);
                            }
                            sh.u = dot(mean - sh.o, d) < 0 ? d * -1.f : d;
                        }
                        sh.e1 = normalize(cross(sh.u, std::abs(sh.u.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0}));
                        sh.e2 = cross(sh.u, sh.e1);
                        float lo = 1e9f, hi = -1e9f;
                        for (const V3& p : pts) {
                            const float a = dot(p - sh.o, sh.u);
                            lo = std::min(lo, a), hi = std::max(hi, a);
                        }
                        sh.a0   = std::floor(lo / DA) * DA;
                        sh.rows = (int)std::floor((hi - sh.a0) / DA) + 1;
                        std::vector<float> raw((size_t)sh.rows * NB, 0.f);
                        for (const V3& p : pts) {
                            const V3    d = p - sh.o;
                            const float a = dot(d, sh.u);
                            const V3    q = d - sh.u * a;
                            const int   r = std::clamp((int)std::floor((a - sh.a0) / DA), 0, sh.rows - 1);
                            const int   b = ((int)std::floor((std::atan2(dot(q, sh.e2), dot(q, sh.e1)) + 3.14159265f) / 6.2831853f * NB) % NB + NB) % NB;
                            raw[(size_t)r * NB + b] = std::max(raw[(size_t)r * NB + b], length(q));
                        }
                        sh.out.assign(raw.size(), 0.f);
                        for (int r = 0; r < sh.rows; ++r)
                            for (int b = 0; b < NB; ++b)
                                for (int d = -1; d <= 1; ++d)
                                    sh.out[(size_t)r * NB + b] = std::max(sh.out[(size_t)r * NB + b], raw[(size_t)r * NB + (b + d + NB) % NB]);
                        bodyShapes.push_back(std::move(sh));
                    }
                    std::string head = "# time", names;
                    for (const auto& k : bodyKinds)
                        head += std::format(" [{}: inside, deepest, bone]", k);
                    fprintf(bodyClips, "%s\n", head.c_str());
                    for (const auto& sh : bodyShapes)
                        names += std::format(" {} ({:.2f} m)", model->nodes[J[sh.joint].node].name, sh.rows * DA);
                    fprintf(stderr, "bodyclip: %zu bones' skin:%s\n", bodyShapes.size(), names.c_str());
                }
                // (the springs as the animation has them, physics off: what's inside by design counts only as far as
                // physics takes it deeper)
                auto skinned = [&](const std::vector<float>& JM, size_t v) {
                    const auto& vx = model->vertices[v];
                    V3          p{};
                    for (int k = 0; k < 4; ++k)
                        if (vx.weights[k]) {
                            const float* m = &JM[vx.joints[k] * 12];
                            p += V3{m[0] * vx.pos[0] + m[1] * vx.pos[1] + m[2] * vx.pos[2] + m[3], m[4] * vx.pos[0] + m[5] * vx.pos[1] + m[6] * vx.pos[2] + m[7],
                                    m[8] * vx.pos[0] + m[9] * vx.pos[1] + m[10] * vx.pos[2] + m[11]} *
                                (vx.weights[k] / 255.f);
                        }
                    return p;
                };
                auto inverses = [&](const std::vector<float>& JM) {
                    std::vector<M4> inv(bodyShapes.size());
                    for (size_t s = 0; s < bodyShapes.size(); ++s) {
                        M4 m = M4::identity();
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 4; ++c)
                                m.m[c * 4 + r] = JM[bodyShapes[s].joint * 12 + r * 4 + c];
                        inv[s] = m.inverse();
                    }
                    return inv;
                };
                const std::vector<M4> inA = inverses(anim.joints()), inS = inverses(still.joints());
                auto                  depth = [&](const std::vector<M4>& inv, const V3& p, int& at) {
                    float most = -1.f;
                    for (size_t s = 0; s < bodyShapes.size(); ++s) {
                        const auto& sh = bodyShapes[s];
                        const V3    d  = inv[s].point(p) - sh.o;
                        const float a  = dot(d, sh.u);
                        const int   r  = (int)std::floor((a - sh.a0) / DA);
                        if (r < 0 || r >= sh.rows)
                            continue;
                        const V3    q  = d - sh.u * a;
                        const float rq = length(q);
                        if (rq < 0.01f)
                            continue; // (on the line itself which way it is from it says nothing)
                        const int b = ((int)std::floor((std::atan2(dot(q, sh.e2), dot(q, sh.e1)) + 3.14159265f) / 6.2831853f * NB) % NB + NB) % NB;
                        if (const float in = sh.out[(size_t)r * NB + b] - rq; in > most)
                            most = in, at = (int)s;
                    }
                    return most;
                };
                std::vector<int>   inside(bodyKinds.size(), 0), bone(bodyKinds.size(), -1);
                std::vector<float> deep(bodyKinds.size(), 0.f);
                for (size_t v = 0; v < model->vertices.size(); ++v) {
                    const int k = vertexBodyKind[v];
                    if (k < 0)
                        continue;
                    int         at = -1, was = -1;
                    const float d  = depth(inA, skinned(anim.joints(), v), at);
                    if (d <= 0.005f)
                        continue;
                    const float more = d - std::max(depth(inS, skinned(still.joints(), v), was), 0.f);
                    if (more > 0.005f) {
                        ++inside[k];
                        if (more > deep[k])
                            deep[k] = more, bone[k] = at;
                    }
                }
                std::string line = std::format("{:.4f}", time);
                for (size_t k = 0; k < bodyKinds.size(); ++k)
                    line += std::format(" {} {:.4f} {}", inside[k], deep[k], bone[k] >= 0 ? model->nodes[J[bodyShapes[bone[k]].joint].node].name : "-");
                fprintf(bodyClips, "%s\n", line.c_str());
            }
            if (walkLog && model) {
                // time, the body's feet (the box's height), its velocity up, on the ground, the ground under it, the
                // avatar's lift, the height it's seen at; then per foot (left, right) its heel's and ball's height over
                // what's under them (< 0: inside it; nan: nothing near); the animator's state; the gait's status
                const auto  under = groundUnder(world.collision, body.feet.x, body.feet.z, body.feet.y);
                std::string clear;
                const M4    toWorld = M4::trs(feet + V3{0, anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1}) * model->fix;
                for (int sd = 0; sd < 2; ++sd) {
                    const int n = model->human[sd ? HB_R_FOOT : HB_L_FOOT];
                    if (n < 0)
                        continue;
                    M4 rest = M4::identity();
                    for (int k = n; k >= 0; k = model->nodes[k].parent)
                        rest = model->nodes[k].rest.matrix() * rest;
                    const M4 carry = toWorld * anim.globals()[n] * rest.inverse() * model->fix.inverse();
                    const V3 ankle = model->fix.point({rest.m[12], rest.m[13], rest.m[14]});
                    for (const V3& off : {model->feet[sd].heel, model->feet[sd].ball}) {
                        const V3 w = carry.point(ankle + off);
                        // (the first surface down from 0.25 m above it, that's under it: inside a step, that step's top)
                        SRayHit h;
                        const bool hit = world.collision.raycast(w + V3{0, 0.25f, 0}, {0, -1, 0}, 1.f, h) && h.normal.y > 0.5f;
                        clear += std::format(" {:.4f}", hit ? h.t - 0.25f : NAN);
                    }
                }
                fprintf(walkLog, "%.4f %.4f %.4f %.4f %.3f %d %.4f %.3f %.4f%s|%s|%s\n", time, body.feet.x, body.feet.y, body.feet.z, mo.vy, mo.onGround ? 1 : 0, under.value_or(NAN),
                        anim.lift(), feet.y, clear.c_str(), anim.playing().c_str(), anim.gaitStatus().c_str());
            }
            if (trace && model) {
                // each foot's heel and ball in the world, carried by the foot as the skin is
                const M4    toWorld = M4::trs(feet + V3{0, anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1}) * model->fix;
                std::string line    = std::format("{:.4f} {:.4f} {:.4f} {:.4f} {:.3f}", time, feet.x, feet.z, bodyYaw, mo.speed);
                for (int sd = 0; sd < 2; ++sd) {
                    const int n = model->human[sd ? HB_R_FOOT : HB_L_FOOT];
                    if (n < 0)
                        continue;
                    M4 rest = M4::identity();
                    for (int k = n; k >= 0; k = model->nodes[k].parent)
                        rest = model->nodes[k].rest.matrix() * rest;
                    const M4 carry = toWorld * anim.globals()[n] * rest.inverse() * model->fix.inverse();
                    const V3 ankle = model->fix.point({rest.m[12], rest.m[13], rest.m[14]});
                    for (const V3& off : {V3{}, model->feet[sd].heel, model->feet[sd].ball}) {
                        const V3 w = carry.point(ankle + off);
                        line += std::format(" {:.4f} {:.4f} {:.4f}", w.x, w.y, w.z);
                    }
                }
                fprintf(trace, "%s %s\n", line.c_str(), anim.gaitStatus().c_str());
            }
            if (springClips && model) {
                // the skin as drawn; round a vertical line through the hips, by height (1 cm) and bearing (5°), how far out
                // the body goes (not its springs, arms or head); a spring's vertex nearer the line than that is inside
                if (vertexSpring.size() != model->vertices.size()) {
                    springKinds.clear();
                    std::vector<int> kindOf(model->springs.size());
                    for (size_t s = 0; s < model->springs.size(); ++s) {
                        const std::string k  = model->springs[s].name.substr(0, model->springs[s].name.find('.'));
                        const auto        it = std::ranges::find(springKinds, k);
                        kindOf[s]            = (int)(it - springKinds.begin());
                        if (it == springKinds.end())
                            springKinds.push_back(k);
                    }
                    std::vector<int> nodeKind(model->nodes.size(), -1), joint(model->nodes.size(), -1);
                    for (const auto& jt : model->springJoints)
                        joint[jt.node] = kindOf[jt.spring];
                    const int lu = model->human[HB_L_UPPER_ARM], ru = model->human[HB_R_UPPER_ARM], nk = model->human[HB_NECK] >= 0 ? model->human[HB_NECK] : model->human[HB_HEAD];
                    for (size_t k = 0; k < model->nodes.size(); ++k) {
                        const int p = model->nodes[k].parent;
                        nodeKind[k] = joint[k] >= 0 ? joint[k] : p >= 0 && nodeKind[p] >= 0 ? nodeKind[p] : (int)k == lu || (int)k == ru || (int)k == nk ? -2 : p >= 0 ? nodeKind[p] : -1;
                    }
                    vertexSpring.resize(model->vertices.size());
                    for (size_t v = 0; v < model->vertices.size(); ++v) {
                        const auto& vx   = model->vertices[v];
                        int         most = 0;
                        for (int k = 1; k < 4; ++k)
                            if (vx.weights[k] > vx.weights[most])
                                most = k;
                        vertexSpring[v] = nodeKind[model->joints[vx.joints[most]].node];
                    }
                    std::string head = "# time";
                    for (const auto& k : springKinds)
                        head += std::format(" [{}: inside, deepest]", k);
                    fprintf(springClips, "%s\n", head.c_str());
                }
                std::vector<uint8_t> shown(model->vertices.size(), 0);
                for (const auto& b : model->batches)
                    if (b.part < 0 || b.part >= (int)anim.partsShown().size() || anim.partsShown()[b.part])
                        for (uint32_t i = b.first; i < b.first + b.count; ++i)
                            shown[model->indices[i]] = 1;
                // (and the springs as the animation has them, physics off: what's inside by design, like a tie's band under
                // the collar, counts only as far as physics takes it deeper)
                const M4        toWorld = M4::trs(feet + V3{0, anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1}) * model->fix;
                std::vector<V3> at(model->vertices.size()), still0(model->vertices.size());
                for (size_t v = 0; v < at.size(); ++v) {
                    const auto& vx = model->vertices[v];
                    if (!shown[v] || vertexSpring[v] == -2)
                        continue;
                    for (int w = 0; w < (vertexSpring[v] >= 0 ? 2 : 1); ++w) {
                        const auto& J = w ? still.joints() : anim.joints();
                        V3          p{};
                        for (int k = 0; k < 4; ++k)
                            if (vx.weights[k]) {
                                const float* m = &J[vx.joints[k] * 12];
                                p += V3{m[0] * vx.pos[0] + m[1] * vx.pos[1] + m[2] * vx.pos[2] + m[3], m[4] * vx.pos[0] + m[5] * vx.pos[1] + m[6] * vx.pos[2] + m[7],
                                        m[8] * vx.pos[0] + m[9] * vx.pos[1] + m[10] * vx.pos[2] + m[11]} *
                                    (vx.weights[k] / 255.f);
                            }
                        (w ? still0 : at)[v] = toWorld.point(p);
                    }
                }
                const M4&       hg   = anim.globals()[model->human[HB_HIPS]];
                const V3        hips = toWorld.point({hg.m[12], hg.m[13], hg.m[14]});
                constexpr int   NH = 220, NA = 72;
                constexpr float H0 = -1.f, DH = 0.01f;
                std::vector<float> out(NH * NA, 0.f);
                auto               bin = [&](const V3& p, int& h, int& a) {
                    h = (int)std::floor((p.y - hips.y - H0) / DH);
                    a = ((int)std::floor((std::atan2(p.z - hips.z, p.x - hips.x) + 3.14159265f) / 6.2831853f * NA) % NA + NA) % NA;
                    return h >= 0 && h < NH;
                };
                for (size_t v = 0; v < at.size(); ++v)
                    if (int h, a; shown[v] && vertexSpring[v] == -1 && bin(at[v], h, a))
                        out[h * NA + a] = std::max(out[h * NA + a], std::hypot(at[v].x - hips.x, at[v].z - hips.z));
                std::vector<int>   inside(springKinds.size(), 0);
                std::vector<float> deep(springKinds.size(), 0.f);
                auto depth = [&](const V3& p) {
                    int   h, a;
                    float r = 0;
                    if (!bin(p, h, a) || std::hypot(p.x - hips.x, p.z - hips.z) < 0.06f)
                        return 0.f; // (on the line itself which way it is from it says nothing)
                    for (int d = -1; d <= 1; ++d)
                        r = std::max(r, out[h * NA + (a + d + NA) % NA]);
                    return r - std::hypot(p.x - hips.x, p.z - hips.z);
                };
                for (size_t v = 0; v < at.size(); ++v) {
                    if (!shown[v] || vertexSpring[v] < 0)
                        continue;
                    if (const float d = depth(at[v]), more = d - std::max(depth(still0[v]), 0.f); d > 0.005f && more > 0.005f) {
                        ++inside[vertexSpring[v]];
                        deep[vertexSpring[v]] = std::max(deep[vertexSpring[v]], more);
                    }
                }
                std::string line = std::format("{:.4f}", time);
                for (size_t k = 0; k < springKinds.size(); ++k)
                    line += std::format(" {} {:.4f}", inside[k], deep[k]);
                fprintf(springClips, "%s\n", line.c_str());
            }
            if ((bones || clips) && model) {
                const M4 toWorld = M4::trs(feet + V3{0, anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1}) * model->fix;
                if (bones) {
                    // time, the body's yaw, then each humanoid bone's joint in the world (nan: the avatar has none)
                    std::string line = std::format("{:.4f} {:.4f}", time, bodyYaw);
                    for (int b = 0; b < HB_COUNT; ++b) {
                        const int n = model->human[b];
                        const V3  p = n >= 0 ? toWorld.point({anim.globals()[n].m[12], anim.globals()[n].m[13], anim.globals()[n].m[14]}) : V3{NAN, NAN, NAN};
                        line += std::format(" {:.4f} {:.4f} {:.4f}", p.x, p.y, p.z);
                    }
                    fprintf(bones, "%s\n", line.c_str());
                }
                if (clips) {
                    // the skin as drawn; round a vertical line through the hips, by height (1 cm) and bearing (5°), how
                    // far out the body goes (the skirt with it); an arm's vertex nearer the line than that is inside
                    if (vertexPart.size() != model->vertices.size()) {
                        std::vector<int8_t> nodePart(model->nodes.size(), 0);
                        // (the forearms and hands; the upper arms, the head with its hair, and what constraints turn after
                        // something else, count as neither)
                        const int           lu = model->human[HB_L_UPPER_ARM], ru = model->human[HB_R_UPPER_ARM], nk = model->human[HB_NECK] >= 0 ? model->human[HB_NECK] : model->human[HB_HEAD];
                        const int           lf = model->human[HB_L_LOWER_ARM], rf = model->human[HB_R_LOWER_ARM];
                        for (const auto& nc : model->constraints)
                            if (nc.type != SNodeConstraint::ROLL && nc.node >= 0)
                                nodePart[nc.node] = -1;
                        for (size_t k = 0; k < model->nodes.size(); ++k) {
                            const int p = model->nodes[k].parent;
                            nodePart[k] = (int)k == lf ? 1 : (int)k == rf ? 2 : (int)k == lu || (int)k == ru || (int)k == nk || nodePart[k] < 0 ? -1 : p >= 0 ? nodePart[p] : 0;
                        }
                        vertexPart.resize(model->vertices.size());
                        for (size_t v = 0; v < model->vertices.size(); ++v) {
                            const auto& vx   = model->vertices[v];
                            int         most = 0;
                            for (int k = 1; k < 4; ++k)
                                if (vx.weights[k] > vx.weights[most])
                                    most = k;
                            vertexPart[v] = nodePart[model->joints[vx.joints[most]].node];
                        }
                    }
                    const auto& J = anim.joints();
                    std::vector<V3> at(model->vertices.size());
                    for (size_t v = 0; v < at.size(); ++v) {
                        const auto& vx = model->vertices[v];
                        if (vertexPart[v] < 0)
                            continue;
                        V3 p{};
                        for (int k = 0; k < 4; ++k)
                            if (vx.weights[k]) {
                                const float* m = &J[vx.joints[k] * 12];
                                const float  w = vx.weights[k] / 255.f;
                                p += V3{m[0] * vx.pos[0] + m[1] * vx.pos[1] + m[2] * vx.pos[2] + m[3], m[4] * vx.pos[0] + m[5] * vx.pos[1] + m[6] * vx.pos[2] + m[7],
                                        m[8] * vx.pos[0] + m[9] * vx.pos[1] + m[10] * vx.pos[2] + m[11]} *
                                    w;
                            }
                        at[v] = toWorld.point(p);
                    }
                    const M4&       hg   = anim.globals()[model->human[HB_HIPS]];
                    const V3        hips = toWorld.point({hg.m[12], hg.m[13], hg.m[14]});
                    constexpr int   NH = 120, NA = 72;
                    constexpr float H0 = -0.8f, DH = 0.01f;
                    std::vector<float> out(NH * NA, 0.f);
                    auto               bin = [&](const V3& p, int& h, int& a) {
                        h = (int)std::floor((p.y - hips.y - H0) / DH);
                        a = ((int)std::floor((std::atan2(p.z - hips.z, p.x - hips.x) + 3.14159265f) / 6.2831853f * NA) % NA + NA) % NA;
                        return h >= 0 && h < NH;
                    };
                    for (size_t v = 0; v < at.size(); ++v)
                        if (int h, a; vertexPart[v] == 0 && bin(at[v], h, a))
                            out[h * NA + a] = std::max(out[h * NA + a], std::hypot(at[v].x - hips.x, at[v].z - hips.z));
                    std::string line = std::format("{:.4f}", time);
                    for (int s = 1; s <= 2; ++s) {
                        int   inside = 0;
                        float deep   = 0;
                        V3    where{};
                        for (size_t v = 0; v < at.size(); ++v) {
                            int h, a;
                            if (vertexPart[v] != s || !bin(at[v], h, a))
                                continue;
                            float r = 0;
                            for (int d = -1; d <= 1; ++d)
                                r = std::max(r, out[h * NA + (a + d + NA) % NA]);
                            const float depth = r - std::hypot(at[v].x - hips.x, at[v].z - hips.z);
                            if (depth > 0.005f) {
                                ++inside;
                                if (depth > deep)
                                    deep = depth, where = at[v] - hips;
                            }
                        }
                        // (where the deepest is, from the hips: across the body's right, up, ahead)
                        const V3 fw{std::sin(bodyYaw), 0, -std::cos(bodyYaw)}, rt{std::cos(bodyYaw), 0, std::sin(bodyYaw)};
                        line += std::format(" {} {:.4f} {:.3f} {:.3f} {:.3f}", inside, deep, dot(where, rt), where.y, dot(where, fw));
                    }
                    fprintf(clips, "%s\n", line.c_str());
                }
            }
            if (hairClips && model && model->clearance.measured) {
                // the hair (what springs swing under the head), as drawn; each arm as the round shape its skin goes out to
                // round its bones at rest (SBodyClearance: a sleeve's cuff with it), by quarter along the upper arm, the
                // forearm and the hand (toward its middle finger); a hair vertex nearer an arm's bone than that is inside
                if (vertexHair.size() != model->vertices.size()) {
                    const size_t         n = model->nodes.size();
                    std::vector<uint8_t> spring(n, 0), head(n, 0);
                    for (const auto& jt : model->springJoints)
                        spring[jt.node] = 1;
                    for (size_t k = 0; k < n; ++k)
                        if (const int p = model->nodes[k].parent; p >= 0) {
                            spring[k] |= spring[p];
                            head[k] = p == model->human[HB_HEAD] || head[p];
                        }
                    vertexHair.assign(model->vertices.size(), 0);
                    for (size_t v = 0; v < model->vertices.size(); ++v) {
                        const auto& vx   = model->vertices[v];
                        int         most = 0;
                        for (int k = 1; k < 4; ++k)
                            if (vx.weights[k] > vx.weights[most])
                                most = k;
                        const int nd  = model->joints[vx.joints[most]].node;
                        vertexHair[v] = spring[nd] && head[nd];
                    }
                    fprintf(hairClips, "# time, then per arm (left, right): hair vertices inside it, the deepest (m), the node that one hangs from\n");
                }
                std::vector<uint8_t> shown(model->vertices.size(), 0);
                for (const auto& b : model->batches)
                    if (b.part < 0 || b.part >= (int)anim.partsShown().size() || anim.partsShown()[b.part])
                        for (uint32_t k = b.first; k < b.first + b.count; ++k)
                            shown[model->indices[k]] = 1;
                const M4    toWorld = M4::trs(feet + V3{0, anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1}) * model->fix;
                const auto& J       = anim.joints();
                const auto& G       = anim.globals();
                auto        jointAt = [&](int nd) { return toWorld.point({G[nd].m[12], G[nd].m[13], G[nd].m[14]}); };
                std::string line    = std::format("{:.4f}", time);
                for (int sd = 0; sd < 2; ++sd) {
                    const int ua = model->human[sd ? HB_R_UPPER_ARM : HB_L_UPPER_ARM];
                    if (ua < 0 || model->human[sd ? HB_R_LOWER_ARM : HB_L_LOWER_ARM] < 0 || model->human[sd ? HB_R_HAND : HB_L_HAND] < 0) {
                        line += " 0 0.0000 -";
                        continue;
                    }
                    const V3 sh = jointAt(ua), el = jointAt(model->human[sd ? HB_R_LOWER_ARM : HB_L_LOWER_ARM]), wr = jointAt(model->human[sd ? HB_R_HAND : HB_L_HAND]);
                    V3       tip = wr + normalize(wr - el) * model->clearance.hand[sd];
                    for (int f : {FINGER_MIDDLE, FINGER_INDEX, FINGER_RING, FINGER_LITTLE})
                        if (const int nd = model->human[fingerBone(sd, f, 0)]; nd >= 0) {
                            tip = wr + normalize(jointAt(nd) - wr) * model->clearance.hand[sd];
                            break;
                        }
                    const V3 seg[3][2] = {{sh, el}, {el, wr}, {wr, tip}};
                    int      inside    = 0, deepNode = -1;
                    float    deep      = 0;
                    for (size_t v = 0; v < model->vertices.size(); ++v) {
                        if (!vertexHair[v] || !shown[v])
                            continue;
                        const auto& vx = model->vertices[v];
                        V3          p{};
                        for (int k = 0; k < 4; ++k)
                            if (vx.weights[k]) {
                                const float* m = &J[vx.joints[k] * 12];
                                p += V3{m[0] * vx.pos[0] + m[1] * vx.pos[1] + m[2] * vx.pos[2] + m[3], m[4] * vx.pos[0] + m[5] * vx.pos[1] + m[6] * vx.pos[2] + m[7],
                                        m[8] * vx.pos[0] + m[9] * vx.pos[1] + m[10] * vx.pos[2] + m[11]} *
                                    (vx.weights[k] / 255.f);
                            }
                        p           = toWorld.point(p);
                        float depth = 0;
                        for (int k = 0; k < 3; ++k) {
                            const V3    a = seg[k][0], d = seg[k][1] - seg[k][0];
                            const float l2 = dot(d, d);
                            if (l2 < 1e-8f)
                                continue;
                            const float t = dot(p - a, d) / l2;
                            if (t < 0.f || t > 1.f)
                                continue;
                            depth = std::max(depth, model->clearance.arm[sd][k][std::clamp((int)(t * 4.f), 0, 3)] - length(p - a - d * t));
                        }
                        if (depth > 0.005f && ++inside && depth > deep) {
                            int most = 0;
                            for (int k = 1; k < 4; ++k)
                                if (vx.weights[k] > vx.weights[most])
                                    most = k;
                            deep = depth, deepNode = model->joints[vx.joints[most]].node;
                        }
                    }
                    std::string node = deepNode >= 0 ? model->nodes[deepNode].name : "-";
                    std::ranges::replace(node, ' ', '_');
                    line += std::format(" {} {:.4f} {}", inside, deep, node);
                }
                fprintf(hairClips, "%s\n", line.c_str());
            }
        }
    };
    // how far each spring's bones are turned from where the animation has them, degrees
    auto swing = [&] {
        if (!model)
            return;
        auto m3 = [](const float* j, double m[3][3]) {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    m[r][c] = j[r * 4 + c];
        };
        std::string line;
        for (size_t s = 0; s < model->springs.size(); ++s) {
            double sum = 0, most = 0;
            int    n   = 0;
            for (const auto& jt : model->springJoints) {
                if (jt.spring != (int)s)
                    continue;
                for (size_t j = 0; j < model->joints.size(); ++j)
                    if (model->joints[j].node == jt.node) {
                        double a[3][3], b[3][3];
                        m3(&anim.joints()[j * 12], a);
                        m3(&still.joints()[j * 12], b);
                        // a b^-1 = the turn (both carry the same inverse bind)
                        const double det = b[0][0] * (b[1][1] * b[2][2] - b[1][2] * b[2][1]) - b[0][1] * (b[1][0] * b[2][2] - b[1][2] * b[2][0]) +
                            b[0][2] * (b[1][0] * b[2][1] - b[1][1] * b[2][0]);
                        if (std::abs(det) < 1e-20)
                            break;
                        double inv[3][3];
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c) {
                                const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
                                inv[r][c]    = (b[r1][c1] * b[r2][c2] - b[r1][c2] * b[r2][c1]) / det;
                            }
                        double rel[3][3], tr = 0, sc = 0;
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c) {
                                rel[r][c] = 0;
                                for (int k = 0; k < 3; ++k)
                                    rel[r][c] += a[r][k] * inv[k][c];
                            }
                        for (int r = 0; r < 3; ++r) {
                            tr += rel[r][r];
                            sc += rel[0][r] * rel[0][r];
                        }
                        tr /= std::sqrt(sc);
                        const double ang = std::acos(std::clamp((tr - 1) / 2, -1.0, 1.0)) * 180 / 3.14159265;
                        sum += ang;
                        most = std::max(most, ang);
                        ++n;
                        break;
                    }
            }
            if (n)
                line += std::format(" {}({})={:.1f}/{:.1f}", model->springs[s].name.empty() ? std::to_string(s) : model->springs[s].name, n, sum / n, most);
        }
        fprintf(stderr, "swing (mean/max deg):%s\n", line.empty() ? " no springs" : line.c_str());
    };

    std::string attackClip, attackClipFirst; // --attackclip
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        // laid out for the --size, as the plugin's every frame does it: the mouse moves its cursor by logical pixels
        menu.layout(W, H, 1);
        if (a == "--attackclip") { // file [first person's file]: VRM animations for the attacks of the avatars loaded after it
            need(i, 1);
            attackClip      = argv[++i];
            attackClipFirst = i + 1 < argc && argv[i + 1][0] != '-' ? argv[++i] : "";
        } else if (a == "--avatar") {
            need(i, 1);
            SAvatarRequest req{argv[++i], height};
            req.attack        = attackClip;
            req.attackFirst   = attackClipFirst;
            SAvatarResult res = guardedLoad(loadAvatar, req);
            for (auto& l : res.log)
                fprintf(stderr, "[load] %s\n", l.c_str());
            if (!res.model) {
                fprintf(stderr, "avatar failed: %s\n", res.error.c_str());
                return 1;
            }
            model = res.model;
            anim.reset(model);
            ctl.avatar = model;
            ctl.loaded();
            still.setPhysics(false);
            still.reset(model);
            fprintf(stderr, "avatar %s: %zu triangles, %zu joints, %.2f m, rig %s\n", model->name.c_str(), model->triangles, model->joints.size(), model->height,
                    model->humanFrom.c_str());
        } else if (a == "--tpose") { // stand in the rest pose: an empty idle clip
            SAnimClip c;
            c.name = "tpose";
            model->clips.push_back(c);
            model->clipFor[CLIP_IDLE] = (int)model->clips.size() - 1;
            anim.reset(model);
            still.reset(model);
        } else if (a == "--move") { // the feet's velocity in the world, m/s: x z
            need(i, 2);
            move = {(float)atof(argv[i + 1]), 0, (float)atof(argv[i + 2])};
            i += 2;
        } else if (a == "--accel") {
            need(i, 1);
            accel = atof(argv[++i]);
        } else if (a == "--decel") {
            need(i, 1);
            decel = atof(argv[++i]);
        } else if (a == "--turnback") {
            need(i, 1);
            turnBack = atof(argv[++i]);
        } else if (a == "--jump") { // like the plugin's: 6.3 m/s up, 20 m/s² down
            ground      = jumping ? ground : feet.y;
            vel.y       = 6.3f;
            mo.onGround = false;
            jumping     = true;
        } else if (a == "--turn") { // the body's, degrees a second
            need(i, 1);
            turn = atof(argv[++i]);
        } else if (a == "--dt") { // seconds a frame
            need(i, 1);
            frameDt = atof(argv[++i]);
        } else if (a == "--physics") {
            need(i, 1);
            anim.setPhysics(atoi(argv[++i]) != 0);
        } else if (a == "--gaitstyle") { // 1|0: the walk's and run's body from the avatar's clips (made in Blender), or the walking's own
            need(i, 1);
            anim.setGaitStyle(atoi(argv[++i]) != 0);
        } else if (a == "--swing") {
            swing();
        } else if (a == "--springs") { // each spring: its bones, colliders and limit; where its first bone's limit points (the
                                       // avatar's frame: x right, y up, -z ahead) and its bones that leave out a collider
                                       // of the body's they start inside of
            if (!model)
                continue;
            static constexpr const char* LIMIT[] = {"none", "cone", "hinge", "spherical"};
            for (size_t s = 0; s < model->springs.size(); ++s) {
                int         n = 0, in = 0, first = -1;
                std::string from;
                for (size_t j = 0; j < model->springJoints.size(); ++j)
                    if (const auto& jt = model->springJoints[j]; jt.spring == (int)s) {
                        ++n;
                        in += jt.startsIn ? 1 : 0;
                        if (first < 0)
                            first = (int)j;
                    }
                if (first < 0)
                    continue;
                const auto& J    = model->springJoints[first];
                const M4&   g    = anim.globals()[J.node];
                const V3    axis = normalize(model->fix.dir(g.dir(J.limitFrame.rotate({0, 1, 0}))));
                const V3    bone = normalize(model->fix.dir(g.dir(J.tail))), at = model->fix.point({g.m[12], g.m[13], g.m[14]});
                fprintf(stderr, "spring %s: %d bones, %zu colliders, limit %s %.1f %.1f, at %.3f %.3f %.3f, bone %.3f %.3f %.3f, limit's y %.3f %.3f %.3f (%.1f deg off), %d start inside the body's\n",
                        model->springs[s].name.c_str(), n, model->springs[s].colliders.size(), LIMIT[J.limit], J.limitA * 57.29578f, J.limitB * 57.29578f, at.x, at.y, at.z,
                        bone.x, bone.y, bone.z, axis.x, axis.y, axis.z, std::acos(std::clamp(dot(axis, bone), -1.f, 1.f)) * 57.29578f, in);
            }
        } else if (a == "--limits") { // how far the bones with a limit are out of it now: the worst (degrees) and its spring;
                                      // worked out here on its own, from the limit's frame (the parent as it swings, the bone
                                      // as the animation turns it) and the cone's, hinge's or spherical limit's rules
            if (!model)
                continue;
            auto rot = [](const M4& m) {
                const V3 x = normalize(V3{m.m[0], m.m[1], m.m[2]}), y = normalize(V3{m.m[4], m.m[5], m.m[6]});
                return Quat::fromBasis(x, y, cross(x, y));
            };
            float       worst = 0;
            std::string at    = "none";
            int         n     = 0;
            for (const auto& jt : model->springJoints) {
                if (jt.limit == LIMIT_NONE)
                    continue;
                const int p     = model->nodes[jt.node].parent;
                const M4  local = p >= 0 ? still.globals()[p].inverse() * still.globals()[jt.node] : still.globals()[jt.node];
                const M4  g     = (p >= 0 ? anim.globals()[p] : M4::identity()) * local;
                const V3  d     = (rot(g) * jt.limitFrame).conj().rotate(normalize(anim.globals()[jt.node].dir(jt.tail)));
                const float A = jt.limitA * 57.29578f, B = jt.limitB * 57.29578f;
                float       out = 0;
                if (jt.limit == LIMIT_CONE)
                    out = std::acos(std::clamp(d.y, -1.f, 1.f)) * 57.29578f - A;
                else if (jt.limit == LIMIT_HINGE) // (off the yz plane, or round in it too far)
                    out = std::max(std::asin(std::min(std::abs(d.x), 1.f)) * 57.29578f, std::abs(std::atan2(d.z, d.y)) * 57.29578f - A);
                else
                    out = std::max(std::abs(std::atan2(d.z, d.y)) * 57.29578f - A, std::asin(std::min(std::abs(d.x), 1.f)) * 57.29578f - B);
                ++n;
                if (out > worst)
                    worst = out, at = model->springs[jt.spring].name;
            }
            fprintf(stderr, "limits: %d bones, the worst %.2f deg out (%s)\n", n, worst, at.c_str());
        } else if (a == "--height") {
            need(i, 1);
            height = atof(argv[++i]);
        } else if (a == "--size") {
            need(i, 1);
            sscanf(argv[++i], "%dx%d", &W, &H);
        } else if (a == "--pos") {
            need(i, 3);
            feet = {(float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3])};
            i += 3;
        } else if (a == "--yaw") { // the body's, degrees
            need(i, 1);
            bodyYaw = rad(atof(argv[++i]));
        } else if (a == "--view") { // camera around the avatar, degrees, 0 = in front
            need(i, 1);
            orbit    = rad(atof(argv[++i]));
            camChest = false;
        } else if (a == "--view-world") { // camera around the avatar at a yaw in the world, not turning with the body (degrees)
            need(i, 1);
            orbit    = rad(atof(argv[++i]));
            camWorld = true;
            camChest = false;
        } else if (a == "--view-chest") { // camera around the avatar at a yaw from where its chest faces (dances turn it), degrees
            need(i, 1);
            orbit    = rad(atof(argv[++i]));
            camChest = true;
            camWorld = false;
        } else if (a == "--pitch") { // camera elevation, degrees
            need(i, 1);
            pitch = atof(argv[++i]);
        } else if (a == "--dist") {
            need(i, 1);
            dist = atof(argv[++i]);
        } else if (a == "--at") { // look at a bone (by node name, or a humanoid bone: lefthand, righthand, head...), then --shift
            need(i, 1);
            aimName = argv[++i];
        } else if (a == "--shift") { // move the look-at point, world meters: x y z
            need(i, 3);
            shift = {(float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3])};
            i += 3;
        } else if (a == "--target") { // look-at height above the feet, -1 = 55% of the avatar
            need(i, 1);
            targetY = atof(argv[++i]);
        } else if (a == "--fov") {
            need(i, 1);
            fov = atof(argv[++i]);
        } else if (a == "--speed") {
            need(i, 1);
            mo.speed = atof(argv[++i]);
            mo.vel   = {};
            speedSet = true;
        } else if (a == "--run") { // 1|0: the player runs (Shift), else walks
            need(i, 1);
            mo.run = atoi(argv[++i]) != 0;
        } else if (a == "--face") { // 1|0: the body turns to where it goes, as in the plugin's third person
            need(i, 1);
            face = atoi(argv[++i]) != 0;
        } else if (a == "--look-cam") { // 1|0: the head looks as the plugin's third person has it (a camera behind, looking -z)
            need(i, 1);
            lookCam = atoi(argv[++i]) != 0;
        } else if (a == "--gait") { // the walking's state
            if (model)
                fprintf(stderr, "gait %s\n", anim.gaitStatus().c_str());
        } else if (a == "--trace") { // file: a line a frame: time, feet x z, yaw, speed, each foot's ankle, heel and ball in the
                                     // world, the walking's state
            need(i, 1);
            if (trace)
                fclose(trace);
            trace = fopen(argv[++i], "w");
        } else if (a == "--bones") { // file: a line a frame: time, the body's yaw, each humanoid bone's joint in the world
            need(i, 1);
            if (bones)
                fclose(bones);
            bones = fopen(argv[++i], "w");
        } else if (a == "--springclip") { // file: a line a frame: time, then per kind of spring (its name up to a '.', as the
                                          // first line lists them) how many of its vertices physics puts inside the body
                                          // (round a vertical line through the hips, not counting the arms and head), more
                                          // than 5 mm deeper than the animation has them, and the most (m)
            need(i, 1);
            if (springClips)
                fclose(springClips);
            springClips = fopen(argv[++i], "w");
            vertexSpring.clear();
        } else if (a == "--bodyclip") { // file: a line a frame: time, then per kind of spring (as --springclip has them) how many
                                        // of its vertices physics puts inside the body more than 5 mm deeper than the
                                        // animation has them, the most (m) and the bone whose skin that is in: each bone's
                                        // skin as it is at rest round a line through it, carried as the bone is now (any
                                        // pose: a dance's, a fall's), the head's and its hair's not counted
            need(i, 1);
            if (bodyClips)
                fclose(bodyClips);
            bodyClips = fopen(argv[++i], "w");
            vertexBodyKind.clear();
        } else if (a == "--springdump") { // file: a line a frame: time, then each spring bone's joint and tail, then each
                                          // collider's two ends, in the world (m); first what they are, a line each
            need(i, 1);
            if (springDump)
                fclose(springDump);
            springDump = fopen(argv[++i], "w");
        } else if (a == "--springtrace") { // file part: a line a frame: time, then each spring bone's tail with the part
                                             // in its name ("": all) in its parent's frame, x y z (model units)
            need(i, 2);
            if (springTrace)
                fclose(springTrace);
            springTrace = fopen(argv[i + 1], "w");
            springPart  = argv[i + 2];
            i += 2;
        } else if (a == "--hairclip") { // file: a line a frame: time, then per arm (left, right) how many of the hair's vertices (what
                                        // springs swing under the head) are inside it (as round as its skin goes out at rest,
                                        // a sleeve's cuff with it), the deepest (m) and the node that one follows most ("-": none)
            need(i, 1);
            if (hairClips)
                fclose(hairClips);
            hairClips = fopen(argv[++i], "w");
            vertexHair.clear();
        } else if (a == "--clip") { // file: a line a frame: time, then per forearm (left, right) how many of its vertices are inside
                                    // the body (a skirt with it), the deepest (m) and where that is from the hips (right, up, ahead)
            need(i, 1);
            if (clips)
                fclose(clips);
            clips = fopen(argv[++i], "w");
        } else if (a == "--walk") { // the plugin's body through the world from here on (walker.cpp): up its stairs, off ledges
            walking         = true;
            body.feet       = feet;
            body.seenY      = NAN;
            body.vel        = {vel.x, 0, vel.z};
            body.onGround   = true;
            body.overlaps   = [&](const V3& f, float h) {
                return world.collision.overlaps({{f.x - SWalker::RADIUS, f.y, f.z - SWalker::RADIUS}, {f.x + SWalker::RADIUS, f.y + h, f.z + SWalker::RADIUS}});
            };
            mo.ground = [&](float x, float z, float y) { return groundUnder(world.collision, x, z, y); };
        } else if (a == "--floors") { // x0 z0 x1 z1 step file: every floor over that grid (a line a cell: x z, the heights up)
            need(i, 6);
            const float x0 = atof(argv[i + 1]), z0 = atof(argv[i + 2]), x1 = atof(argv[i + 3]), z1 = atof(argv[i + 4]), st = atof(argv[i + 5]);
            FILE*       f = fopen(argv[i + 6], "w");
            i += 6;
            std::vector<SRayHit> hits;
            const float          top = world.bounds.max.y + 1.f, depth = top - world.bounds.min.y + 2.f;
            for (float x = x0; x <= x1; x += st)
                for (float z = z0; z <= z1; z += st) {
                    hits.clear();
                    world.collision.raycastAll({x, top, z}, {0, -1, 0}, depth, hits);
                    std::string line = std::format("{:.2f} {:.2f}", x, z);
                    for (const auto& h : hits)
                        if (h.normal.y > 0.7f)
                            line += std::format(" {:.3f}", top - h.t);
                    fprintf(f, "%s\n", line.c_str());
                }
            fclose(f);
        } else if (a == "--walklog") { // file: a line a frame (with --walk): the body's height, on the ground, the gait
            need(i, 1);
            walkLog = fopen(argv[++i], "w");
        } else if (a == "--vy") {
            need(i, 1);
            mo.vy = atof(argv[++i]);
        } else if (a == "--air") {
            mo.onGround = false;
        } else if (a == "--ground") {
            mo.onGround = true;
        } else if (a == "--fly") { // 1|0: flying (the plugin's F; going on as it was going); 0 above the ground: falls to it
            need(i, 1);
            const bool was = mo.flying;
            mo.flying      = atoi(argv[++i]) != 0;
            if (mo.flying && !was && !jumping)
                ground = feet.y;
            if (!mo.flying && was && !walking) {
                if (feet.y > ground + 1e-3f)
                    jumping = true, mo.onGround = false;
                else
                    vel.y = mo.vy = 0, mo.onGround = true;
            }
            if (mo.flying)
                jumping = false;
        } else if (a == "--movey") { // flying: the velocity up the keys ask for, m/s (the plugin's Space 8, Ctrl -8)
            need(i, 1);
            moveY = atof(argv[++i]);
        } else if (a == "--crouch") {
            need(i, 1);
            mo.crouched = atoi(argv[++i]) != 0;
        } else if (a == "--look") { // head, degrees: yaw (right > 0) pitch (up > 0)
            need(i, 2);
            mo.lookYaw   = rad(atof(argv[i + 1]));
            mo.lookPitch = rad(atof(argv[i + 2]));
            i += 2;
        } else if (a == "--viseme") { // name weight: lip sync's viseme (aa ih ou ee oh pp ff ss ch) at that weight, the rest 0, as the
            // plugin hands them to the avatar ("none": all 0)
            need(i, 2);
            const std::string name = argv[++i];
            const float       w    = (float)atof(argv[++i]);
            SVisemes          v{};
            for (int k = 0; k < VISEME_COUNT; ++k)
                if (name == VISEME_NAMES[k])
                    v[k] = w;
            anim.setVisemes(v);
        } else if (a == "--expr") { // name [weight]; "none" clears
            need(i, 1);
            const std::string name = argv[++i];
            float             w    = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                w = atof(argv[++i]);
            }
            const int e = name == "none" ? -1 : model->findExpression(name);
            if (name != "none" && e < 0)
                fprintf(stderr, "no expression %s\n", name.c_str());
            anim.setExpression(e, w);
        } else if (a == "--gesture") { // hand (left|right|both) gesture
            need(i, 2);
            const std::string hand = argv[i + 1];
            const int         g    = gestureFromName(argv[i + 2]);
            i += 2;
            if (g < 0)
                fprintf(stderr, "no gesture %s\n", argv[i]);
            else {
                if (hand != "right")
                    anim.setGesture(0, g);
                if (hand != "left")
                    anim.setGesture(1, g);
            }
        } else if (a == "--toggle") { // name on|off|flip
            need(i, 2);
            const std::string name = argv[i + 1], st = argv[i + 2];
            i += 2;
            const int t = model->findToggle(name);
            if (t < 0)
                fprintf(stderr, "no toggle %s\n", name.c_str());
            else
                anim.setToggle(t, st == "flip" ? !anim.toggle(t) : st == "on");
        } else if (a == "--slider") { // name 0..1|reset; a 2D one: name x y (-1..1 each)
            need(i, 2);
            const int sl = model->findSlider(argv[i + 1]);
            if (sl < 0)
                fprintf(stderr, "no slider %s\n", argv[i + 1]);
            else if (model->sliders[sl].grid && std::string(argv[i + 2]) != "reset") {
                need(i, 3);
                anim.setSlider(sl, (float)atof(argv[i + 2]), (float)atof(argv[i + 3]));
                ++i;
            } else
                anim.setSlider(sl, std::string(argv[i + 2]) == "reset" ? NAN : (float)atof(argv[i + 2]));
            i += 2;
        } else if (a == "--part") { // name 1|0|-1
            need(i, 2);
            const auto found = model->findParts(argv[i + 1]);
            if (found.empty())
                fprintf(stderr, "no part %s\n", argv[i + 1]);
            for (int p : found)
                anim.setPart(p, atoi(argv[i + 2]));
            i += 2;
        } else if (a == "--shape") { // name weight|reset
            need(i, 2);
            const auto found = model->findMorphs(argv[i + 1]);
            if (found.empty())
                fprintf(stderr, "no shape key %s\n", argv[i + 1]);
            for (int m : found)
                anim.setShape(m, std::string(argv[i + 2]) == "reset" ? NAN : (float)atof(argv[i + 2]));
            i += 2;
        } else if (a == "--parts") { // what's shown, the toggles
            if (!model)
                continue;
            std::string line;
            for (size_t p = 0; p < model->parts.size(); ++p)
                line += " " + model->parts[p].name + (anim.partsShown()[p] ? "" : "(hidden)") + "/" + std::to_string(model->parts[p].triangles);
            fprintf(stderr, "parts:%s\n", line.c_str());
            line.clear();
            for (size_t t = 0; t < model->toggles.size(); ++t) {
                std::string gs;
                for (const auto& g : model->toggles[t].groups)
                    gs += (gs.empty() ? "" : ",") + g;
                line += " " + model->toggles[t].name + (gs.empty() ? "" : "[" + gs + "]") + "=" + (anim.toggle((int)t) ? "on" : "off");
            }
            fprintf(stderr, "toggles:%s\n", line.empty() ? " none" : line.c_str());
            line.clear();
            for (size_t s = 0; s < model->sliders.size(); ++s)
                line += model->sliders[s].grid ? std::format(" {}=({:.3f},{:.3f})[2D {}x{}]", model->sliders[s].name, anim.slider((int)s), anim.sliderY((int)s),
                                                             model->sliders[s].grid, model->sliders[s].grid)
                                               : std::format(" {}={:.3f}", model->sliders[s].name, anim.slider((int)s));
            fprintf(stderr, "sliders:%s\n", line.empty() ? " none" : line.c_str());
            line.clear();
            for (size_t v = 0; v < model->variants.size(); ++v)
                line += " " + model->variants[v] + "=" + (anim.variant((int)v) ? "on" : "off");
            fprintf(stderr, "variants:%s\n", line.empty() ? " none" : line.c_str());
            if (const auto* bm = anim.batchMaterials()) {
                line.clear();
                for (size_t b = 0; b < model->batches.size(); ++b)
                    if ((*bm)[b] != model->batches[b].material)
                        line += std::format(" {}:{}->{}", model->parts[model->batches[b].part].name, model->materials[model->batches[b].material].name,
                                            model->materials[(*bm)[b]].name);
                fprintf(stderr, "materials:%s\n", line.empty() ? " as they are" : line.c_str());
            }
        } else if (a == "--blink") {
            need(i, 1);
            anim.setAutoBlink(atoi(argv[++i]) != 0);
        } else if (a == "--light") {
            need(i, 2);
            sky    = atof(argv[i + 1]);
            bounce = atof(argv[i + 2]);
            i += 2;
        } else if (a == "--exposure") {
            need(i, 1);
            exposure = atof(argv[++i]);
        } else if (a == "--frames") {
            need(i, 1);
            step(atoi(argv[++i]));
        } else if (a == "--eyes") { // each eye's turn relative to the head: angle, axis (in the model's bind space)
            if (!model)
                continue;
            auto rot = [&](int hb, float r[3][3]) {
                const int node = model->human[hb];
                for (size_t j = 0; j < model->joints.size(); ++j)
                    if (model->joints[j].node == node && node >= 0) {
                        for (int a = 0; a < 3; ++a) {
                            float len = 0;
                            for (int b = 0; b < 3; ++b)
                                len += anim.joints()[j * 12 + b * 4 + a] * anim.joints()[j * 12 + b * 4 + a];
                            len = std::sqrt(len);
                            for (int b = 0; b < 3; ++b)
                                r[b][a] = anim.joints()[j * 12 + b * 4 + a] / len;
                        }
                        return true;
                    }
                return false;
            };
            float h[3][3], e[3][3];
            if (!rot(HB_HEAD, h)) {
                fprintf(stderr, "eyes: no head joint\n");
                continue;
            }
            for (int hb : {HB_L_EYE, HB_R_EYE}) {
                if (!rot(hb, e)) {
                    fprintf(stderr, "eyes: %s has no joint\n", hb == HB_L_EYE ? "left" : "right");
                    continue;
                }
                float rel[3][3]; // h^T e
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b) {
                        rel[a][b] = 0;
                        for (int k = 0; k < 3; ++k)
                            rel[a][b] += h[k][a] * e[k][b];
                    }
                const float tr  = rel[0][0] + rel[1][1] + rel[2][2];
                const float ang = std::acos(std::clamp((tr - 1) / 2, -1.f, 1.f)) * 180 / 3.14159265f;
                V3          ax{rel[2][1] - rel[1][2], rel[0][2] - rel[2][0], rel[1][0] - rel[0][1]};
                const float l = length(ax);
                if (l > 1e-6f)
                    ax = ax * (1 / l);
                fprintf(stderr, "eyes: %s %.2f deg about (%.2f %.2f %.2f)\n", hb == HB_L_EYE ? "left" : "right", ang, ax.x, ax.y, ax.z);
            }
        } else if (a == "--morphs") { // the morphs that aren't at 0
            if (!model)
                continue;
            std::string line;
            for (size_t m = 0; m < model->morphs.size() && m < anim.morphWeights().size(); ++m)
                if (std::abs(anim.morphWeights()[m]) > 1e-3f)
                    line += " " + model->morphs[m].name + "=" + std::to_string(anim.morphWeights()[m]).substr(0, 5);
            fprintf(stderr, "morphs:%s\n", line.empty() ? " none" : line.c_str());
        } else if (a == "--fingers") { // the finger bones, by name
            static const char* F[] = {"thumb", "index", "middle", "ring", "little"};
            for (int hand = 0; hand < 2; ++hand)
                for (int f = 0; f < FINGER_COUNT; ++f) {
                    std::string line;
                    for (int s = 0; s < 3; ++s) {
                        const int n = model->human[fingerBone(hand, f, s)];
                        line += " " + (n >= 0 ? model->nodes[n].name : std::string("-"));
                    }
                    fprintf(stderr, "%s %s:%s\n", hand ? "right" : "left", F[f], line.c_str());
                }
        } else if (a == "--thumb") { // each finger's bones, rest and posed, in the hand's frame: along, across, palm
            if (!model)
                continue;
            const auto& J = anim.joints();
            auto pos = [&](int node, bool posed, V3& out) {
                for (size_t j = 0; j < model->joints.size(); ++j)
                    if (model->joints[j].node == node && node >= 0) {
                        const M4 ib = model->joints[j].inverseBind.inverse();
                        const V3 bind{ib.m[12], ib.m[13], ib.m[14]};
                        if (!posed) {
                            out = bind;
                            return true;
                        }
                        M4 skin = M4::identity();
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 4; ++c)
                                skin.m[c * 4 + r] = J[j * 12 + r * 4 + c];
                        out = skin.point(bind);
                        return true;
                    }
                return false;
            };
            static const char* F[] = {"thumb", "index", "middle", "ring", "little"};
            for (int hand = 0; hand < 2; ++hand) {
                auto bone = [&](int f, int s) { return model->human[fingerBone(hand, f, s)]; };
                V3 h, a0, a1, mid{};
                if (!pos(model->human[hand ? HB_R_HAND : HB_L_HAND], false, h))
                    continue;
                int count = 0;
                for (int f = 1; f < FINGER_COUNT; ++f)
                    if (V3 q; pos(bone(f, 0), false, q)) {
                        mid += q;
                        ++count;
                    }
                if (!count)
                    continue;
                mid            = mid * (1.f / count);
                const V3 along = normalize(mid - h);
                pos(bone(1, 0), false, a0);
                pos(bone(4, 0), false, a1);
                V3       v      = a0 - a1;
                const V3 across = normalize(v - along * dot(v, along));
                const V3 palm   = cross(along, across) * (hand == 0 ? 1.f : -1.f);
                fprintf(stderr, "%s hand: along (%.2f %.2f %.2f) across (%.2f %.2f %.2f) palm (%.2f %.2f %.2f)\n", hand ? "right" : "left", along.x, along.y,
                        along.z, across.x, across.y, across.z, palm.x, palm.y, palm.z);
                auto in = [&](V3 d) { return V3{dot(d, along), dot(d, across), dot(d, palm)}; };
                for (int f = 0; f < FINGER_COUNT; ++f) {
                    std::string line;
                    for (int posed = 0; posed < 2; ++posed) {
                        line += posed ? " | posed" : " rest";
                        int prev = -1;
                        for (int s = 0; s < 4; ++s) {
                            int n = s < 3 ? bone(f, s) : -1;
                            if (s == 3 && prev >= 0) // the tip: the distal's child
                                for (size_t k = 0; k < model->nodes.size(); ++k)
                                    if (model->nodes[k].parent == prev)
                                        n = (int)k;
                            V3 p0, p1;
                            if (prev >= 0 && n >= 0 && pos(prev, posed, p0) && pos(n, posed, p1)) {
                                const V3 d = in(normalize(p1 - p0));
                                char     b[64];
                                snprintf(b, sizeof b, " (%.2f %.2f %.2f)", d.x, d.y, d.z);
                                line += b;
                            }
                            prev = n;
                        }
                    }
                    if (V3 t, q; pos(bone(f, 0), true, t) && pos(model->human[hand ? HB_R_HAND : HB_L_HAND], true, q))
                        (void)0;
                    fprintf(stderr, "  %s:%s\n", F[f], line.c_str());
                }
            }
        } else if (a == "--emote") { // name|number|file|folder [loop|once]; "stop" stops
            need(i, 1);
            const std::string what = argv[++i];
            int               loop = -1;
            if (i + 1 < argc && (std::string(argv[i + 1]) == "loop" || std::string(argv[i + 1]) == "once"))
                loop = std::string(argv[++i]) == "loop";
            if (what == "stop") {
                anim.stopEmote();
                still.stopEmote();
                continue;
            }
            int e = -1;
            if (what.find('/') != std::string::npos || isEmoteFile(what)) {
                SEmoteResult res = guardedLoad(loadEmotes, SEmoteRequest{what, {what}, model});
                for (auto& l : res.log)
                    fprintf(stderr, "[emote] %s\n", l.c_str());
                if (!res.error.empty())
                    fprintf(stderr, "emote failed: %s\n", res.error.c_str());
                for (const auto& em : res.emotes) {
                    const int k = anim.addEmote(em);
                    still.addEmote(em);
                    fprintf(stderr, "emote %s from %s: %.2f s, %zu channels, %zu faces%s -> %d\n", em->name.c_str(), em->from.c_str(), em->anim.duration,
                            em->anim.channels.size(), em->faces.size(), em->fingers ? ", fingers" : "", k);
                    if (e < 0)
                        e = k;
                }
            } else
                e = anim.findEmote(what);
            if (e < 0)
                fprintf(stderr, "no emote %s\n", what.c_str());
            anim.playEmote(e, loop);
            still.playEmote(e, loop); // (the swing and --springclip are measured against it)
        } else if (a == "--emotes") { // the avatar's
            for (size_t k = 0; k < anim.emotes().size(); ++k) {
                const auto& em = *anim.emotes()[k];
                fprintf(stderr, "emote %zu: %s (%s) %.2f s%s%s%s%s, %zu channels, %zu faces%s, gestures %d %d\n", k + 1, em.name.c_str(), em.from.c_str(), em.anim.duration,
                        em.loop ? " loop" : "", em.hold ? " hold" : "", em.grounded ? " grounded" : "", em.speed != 1 ? std::format(" at {:g}x", em.speed).c_str() : "",
                        em.anim.channels.size(), em.faces.size(), em.eyes.empty() ? "" : ", eyes", em.gesture[0], em.gesture[1]);
            }
        } else if (a == "--menu") { // page, or a path: "gestures/left", "emotes:2"
            need(i, 1);
            if (!menu.show(argv[++i]))
                fprintf(stderr, "no menu page %s\n", argv[i]);
        } else if (a == "--menu-move") { // dx dy, logical pixels
            need(i, 2);
            menu.move(atof(argv[i + 1]), atof(argv[i + 2]));
            i += 2;
        } else if (a == "--menu-scroll") {
            need(i, 1);
            menu.scroll(atoi(argv[++i]));
        } else if (a == "--menu-pick") { // 1-9, 0 = the middle, "cursor" = what it points at
            need(i, 1);
            const std::string n  = argv[++i];
            const auto        it = n == "cursor" ? menu.pick() : menu.pick(atoi(n.c_str()) - 1);
            if (it)
                menuDo(*it);
            else
                fprintf(stderr, "menu at %s\n", menu.open() ? menu.path().c_str() : "(closed)");
        } else if (a == "--menu-close") {
            menu.hide();
        } else if (a == "--ctl") { // "avatar ..." or "menu ...": a hyprctl hypr3d request, as the plugin does it
            need(i, 1);
            const std::string        req = argv[++i];
            std::istringstream       in(req);
            std::vector<std::string> words;
            for (std::string w; in >> w;)
                words.push_back(w);
            std::string rest = req; // after "avatar emote"
            for (int w = 0; w < 2; ++w) {
                const size_t b = rest.find_first_not_of(" \t"), e = b == std::string::npos ? b : rest.find_first_of(" \t", b);
                rest           = e == std::string::npos ? "" : rest.substr(e);
            }
            std::string r;
            if (!words.empty() && words[0] == "avatar")
                r = ctl.command(words, rest, nullptr);
            else if (!words.empty() && words[0] == "menu")
                r = menuCommand(menu, {words.begin() + 1, words.end()}, menuDo);
            fprintf(stderr, "ctl %s -> %s\n", req.c_str(), r.empty() ? "(not a command here)" : r.c_str());
        } else if (a == "--key") { // tab, esc, backspace, enter, 1-9 or an evdev code, as the plugin takes them
            need(i, 1);
            const std::string k    = argv[++i];
            const uint32_t    code = k == "tab" ? 15 : k == "esc" ? 1 : k == "backspace" ? 14 : k == "enter" ? 28 : k.size() == 1 && k[0] >= '1' && k[0] <= '9' ? (uint32_t)(k[0] - '1' + 2) : (uint32_t)atoi(k.c_str());
            if (code == 15) // Tab opens and closes it
                menu.open() ? menu.hide() : (void)menu.show();
            else if (!menuKey(menu, code, menuPick))
                fprintf(stderr, "key %s: not the menu's\n", k.c_str());
        } else if (a == "--click") { // left|right|middle, while the menu is open
            need(i, 1);
            const std::string b = argv[++i];
            if (!menuButton(menu, b == "right" ? 0x111 : b == "middle" ? 0x112 : 0x110, menuPick))
                fprintf(stderr, "click %s: the menu isn't open\n", b.c_str());
        } else if (a == "--wheel") { // notches (down > 0), while the menu is open
            need(i, 1);
            menuWheel(menu, wheel, (float)atof(argv[++i]));
        } else if (a == "--mouse") { // dx dy: the mouse moved (the menu's cursor, while it's open)
            need(i, 2);
            if (menu.open())
                menu.move(atof(argv[i + 1]), atof(argv[i + 2]));
            i += 2;
        } else if (a == "--menu-dt") {
            need(i, 1);
            menuDt = atof(argv[++i]);
        } else if (a == "--status") {
            fprintf(stderr, "playing: %s, lift %.3f\n", anim.playing().c_str(), anim.lift());
            if (model) {
                std::string ws;
                for (size_t k = 0; k < model->morphs.size(); ++k)
                    if (anim.morphWeights()[k] > 0.01f)
                        ws += std::format(" {}={:.2f}", model->morphs[k].name, anim.morphWeights()[k]);
                fprintf(stderr, "morphs:%s\n", ws.c_str());
            }
        } else if (a == "--where") { // node: where in the world it is (its joint's bind point, skinned)
            need(i, 1);
            const std::string nm = argv[++i];
            int               node = -1;
            for (size_t k = 0; model && k < model->nodes.size() && node < 0; ++k)
                if (model->nodes[k].name == nm)
                    node = (int)k;
            bool said = false;
            for (size_t j = 0; node >= 0 && j < model->joints.size() && !said; ++j)
                if (model->joints[j].node == node) {
                    const auto& J    = anim.joints();
                    M4          skin = M4::identity();
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 4; ++c)
                            skin.m[c * 4 + r] = J[j * 12 + r * 4 + c];
                    const M4 ib = model->joints[j].inverseBind.inverse();
                    const M4 toWorld = M4::trs(feet + V3{0, anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1}) * model->fix;
                    const V3 p       = toWorld.point(skin.point({ib.m[12], ib.m[13], ib.m[14]}));
                    fprintf(stderr, "where %s: %.4f %.4f %.4f (feet %.3f %.3f %.3f)\n", nm.c_str(), p.x, p.y, p.z, feet.x, feet.y, feet.z);
                    said = true;
                }
            if (!said)
                fprintf(stderr, "where %s: no such node with a joint\n", nm.c_str());
        } else if (a == "--map") { // path [scale]: a glTF map instead of the courtyard; the feet go to its start
            need(i, 1);
            SMapRequest req;
            req.path     = argv[++i];
            req.compress = plainTextures ? 0 : gl::textureCompression();
            if (i + 1 < argc && argv[i + 1][0] != '-')
                req.scale = atof(argv[++i]);
            const auto t0  = std::chrono::steady_clock::now();
            SMapResult res = guardedLoad(loadMap, req);
            for (const auto& l : res.log)
                fprintf(stderr, "[map] %s\n", l.c_str());
            if (!res.world || !res.error.empty()) {
                fprintf(stderr, "map failed: %s\n", res.error.c_str());
                return 1;
            }
            world = std::move(*res.world);
            if (!renderer.init(world)) {
                fprintf(stderr, "renderer init failed for the map\n");
                return 1;
            }
            feet     = world.spawn;
            bodyYaw  = world.spawnYaw;
            camYaw   = world.spawnYaw * DEG;
            camPitch = 0;
            const auto& d = world.desktop;
            const auto& b = world.bounds;
            fprintf(stderr, "map loaded in %lld ms, scale %g: bounds %.1f %.1f %.1f .. %.1f %.1f %.1f; spawn %.2f %.2f %.2f yaw %.0f; desktop %.2f %.2f %.2f n %.2f %.2f %.2f h %.2f; sun %.2f %.2f %.2f\n",
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(), res.req.scale, b.min.x, b.min.y, b.min.z,
                    b.max.x, b.max.y, b.max.z, feet.x, feet.y, feet.z, camYaw, d.center.x, d.center.y, d.center.z, d.normal.x, d.normal.y, d.normal.z, d.height,
                    world.sunDir.x, world.sunDir.y, world.sunDir.z);
        } else if (a == "--fp") { // first person from the feet: yaw pitch (degrees); 1.65 m up, no body (but its shadow)
            need(i, 2);
            firstPerson = true;
            fpBody      = false;
            eyeSet      = false;
            camYaw      = atof(argv[i + 1]);
            camPitch    = atof(argv[i + 2]);
            i += 2;
        } else if (a == "--fpbody") { // first person with the body: yaw pitch (degrees); the camera in the avatar's eyes, its hands in view
            need(i, 2);
            fpBody = firstPerson = true;
            eyeSet               = false;
            camYaw               = atof(argv[i + 1]);
            camPitch             = atof(argv[i + 2]);
            i += 2;
        } else if (a == "--fpturn") { // yaw pitch, degrees a second: the first person camera turning (the mouse), each frame
            need(i, 2);
            fpTurnYaw   = atof(argv[i + 1]);
            fpTurnPitch = atof(argv[i + 2]);
            i += 2;
        } else if (a == "--fpfollow") { // 1|0: the body turns as in the plugin's first person (else as --turn has it)
            need(i, 1);
            fpFollow = atoi(argv[++i]) != 0;
        } else if (a == "--fpoff") { // the body's first person off (the hands the animation's again), its camera gone too
            fpBody = false;
            mo.fp  = {};
        } else if (a == "--fphands") { // ready|touch|type|hold|down: what the hands do in first person
            need(i, 1);
            const std::string v = argv[++i];
            mo.fp.hands         = v == "touch" ? FPH_TOUCH : v == "type" ? FPH_TYPE : v == "hold" ? FPH_HOLD : v == "down" ? FPH_DOWN : FPH_READY;
        } else if (a == "--fproom") { // meters: how far ahead of the eye there's room for the hands (a wall)
            need(i, 1);
            mo.fp.room = atof(argv[++i]);
        } else if (a == "--fppress") { // a button goes down (touching: the finger pokes), the next frame
            mo.fp.press = true;
        } else if (a == "--fptap") { // left|right: a key goes down for that hand (typing), the next frame
            need(i, 1);
            mo.fp.tap = std::string(argv[++i]) == "left" ? 0 : 1;
        } else if (a == "--attack") { // left|right|next: an attack (the plugin's left click on nothing): that arm swung, or whichever's next
            need(i, 1);
            const std::string v = argv[++i];
            fprintf(stderr, "attack %s: %s\n", v.c_str(), model && anim.attack(v == "left" ? 0 : v == "right" ? 1 : -1) ? "ok" : "none");
        } else if (a == "--attackstatus") { // the attacks' swings, as the plugin's avatar status has them
            if (model)
                fprintf(stderr, "attack: %s\n", anim.attackStatus().c_str());
        } else if (a == "--wrists") { // where each wrist is from the feet, in the avatar's own frame (x right, y up, -z ahead); first
                                      // person's arms; then where the shoulders (the upper arms' joints) are, the same way
            if (!model)
                continue;
            auto at = [&](std::initializer_list<int> bones) {
                std::string line;
                for (int b : bones)
                    if (const int n = model->human[b]; n >= 0) {
                        const V3 p = model->fix.point({anim.globals()[n].m[12], anim.globals()[n].m[13], anim.globals()[n].m[14]}) + V3{0, anim.lift(), 0};
                        line += std::format(" {:.4f} {:.4f} {:.4f}", p.x, p.y, p.z);
                    }
                return line;
            };
            fprintf(stderr, "wrists:%s arms %.3f shoulders%s\n", at({HB_L_HAND, HB_R_HAND}).c_str(), anim.firstPersonArms(), at({HB_L_UPPER_ARM, HB_R_UPPER_ARM}).c_str());
        } else if (a == "--fpstatus") { // first person: the camera, how much is in, and where each hand is on the screen
            if (!model)
                continue;
            const auto  e     = anim.eyes();
            const M4    drawn = M4::trs(feet + V3{0, anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1});
            const V3    eye   = mo.fp.eye;
            const M4    vp    = M4::perspective(rad(fov), (float)W / H, 0.05f, 200.f) * M4::lookAt(eye, eye + forwardFrom(rad(camYaw), rad(camPitch)), {0, 1, 0});
            std::string hands;
            for (int s = 0; s < 2; ++s) {
                for (int b : {HB_L_HAND, HB_L_LOWER_ARM}) {
                    const int n = model->human[b + (s ? HB_R_UPPER_ARM - HB_L_UPPER_ARM : 0)];
                    if (n < 0)
                        continue;
                    const V3    p = drawn.point(model->fix.point(V3{anim.globals()[n].m[12], anim.globals()[n].m[13], anim.globals()[n].m[14]}));
                    const float c[4] = {vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8] * p.z + vp.m[12], vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9] * p.z + vp.m[13], 0,
                                        vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15]};
                    // (the screen from its top left, 0..1 across and down; behind the eye: nan)
                    const float sx = c[3] > 0 ? 0.5f + 0.5f * c[0] / c[3] : NAN, sy = c[3] > 0 ? 0.5f - 0.5f * c[1] / c[3] : NAN;
                    hands += std::format(" {}{} {:.3f} {:.3f} ({:.2f} m)", s ? "right" : "left", b == HB_L_HAND ? "wrist" : "elbow", sx, sy, length(p - eye));
                }
            }
            fprintf(stderr, "fp: on %d weight %.2f arms %.2f eye %.3f %.3f %.3f (%.3f up, eyes %s) yaw %.1f pitch %.1f body %.1f;%s\n", fpBody ? 1 : 0, anim.firstPerson(),
                    anim.firstPersonArms(), eye.x, eye.y, eye.z,
                    eye.y - feet.y, e ? std::format("{:.3f} {:.3f} {:.3f}", drawn.point(*e).x, drawn.point(*e).y, drawn.point(*e).z).c_str() : "none", camYaw, camPitch,
                    bodyYaw * DEG, hands.c_str());
        } else if (a == "--eye") { // first person from a point: x y z yaw pitch
            need(i, 5);
            firstPerson = true;
            eyeSet      = true;
            eyeAt       = {(float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3])};
            camYaw      = atof(argv[i + 4]);
            camPitch    = atof(argv[i + 5]);
            i += 5;
        } else if (a == "--spawn") { // first person at the map's start, looking where it looks
            firstPerson = true;
            eyeSet      = false;
            feet        = world.spawn;
            camYaw      = world.spawnYaw * DEG;
            camPitch    = 0;
        } else if (a == "--desk") { // first person from the start, looking at the middle of the desktop
            firstPerson = true;
            eyeSet      = false;
            feet        = world.spawn;
            const V3 to = world.desktop.center - (feet + V3{0, 1.65f, 0});
            camYaw      = std::atan2(to.x, -to.z) * DEG;
            camPitch    = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z)) * DEG;
        } else if (a == "--stand") { // first person on the floor under x y z: yaw pitch
            need(i, 5);
            const V3 at{(float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3])};
            std::vector<SRayHit> hits;
            world.collision.raycastAll(at + V3{0, 1.f, 0}, {0, -1, 0}, 30.f, hits);
            feet = at;
            for (const auto& h : hits)
                if (h.normal.y > 0.6f) {
                    feet = at + V3{0, 1.f - h.t + 0.02f, 0};
                    break;
                }
            firstPerson = true;
            eyeSet      = false;
            camYaw      = atof(argv[i + 4]);
            camPitch    = atof(argv[i + 5]);
            i += 5;
            fprintf(stderr, "standing at %.2f %.2f %.2f\n", feet.x, feet.y, feet.z);
        } else if (a == "--probe") { // a ray from the first person eye: yaw pitch; prints where it hits
            need(i, 2);
            const V3 eye = eyeSet ? eyeAt : feet + V3{0, 1.65f, 0};
            const V3 d   = forwardFrom(rad(atof(argv[i + 1])), rad(atof(argv[i + 2])));
            SRayHit  h;
            if (world.collision.raycast(eye, d, 200.f, h)) {
                const V3 p = eye + d * h.t;
                fprintf(stderr, "probe %s %s: hit at %.2f m: %.3f %.3f %.3f normal %.3f %.3f %.3f\n", argv[i + 1], argv[i + 2], h.t, p.x, p.y, p.z, h.normal.x, h.normal.y, h.normal.z);
            } else
                fprintf(stderr, "probe %s %s: nothing\n", argv[i + 1], argv[i + 2]);
            i += 2;
        } else if (a == "--scanwalls") { // step ymin ymax: every flat wall a 2.4 m desktop fits on, seen from 3-8 m
            need(i, 3);
            const float step = atof(argv[i + 1]), ymin = atof(argv[i + 2]), ymax = atof(argv[i + 3]);
            i += 3;
            const auto& col = world.collision;
            const SAABB b   = world.bounds;
            const float H = 2.4f, W = H * 16.f / 9.f;
            struct SCand {
                V3 feet, center, normal;
                float dist, back, exposure, score;
            };
            std::vector<SCand> cands;
            std::vector<SRayHit> hits;
            const V3 up{0, 1, 0};
            for (float x = b.min.x; x <= b.max.x; x += step)
                for (float z = b.min.z; z <= b.max.z; z += step) {
                    col.raycastAll({x, b.max.y + 1, z}, {0, -1, 0}, b.size().y + 2, hits);
                    for (const auto& h : hits) {
                        if (h.normal.y < 0.7f)
                            continue;
                        const V3 f{x, b.max.y + 1 - h.t + 0.02f, z};
                        if (f.y < ymin || f.y > ymax)
                            continue;
                        if (col.overlaps({{f.x - 0.3f, f.y + 0.05f, f.z - 0.3f}, {f.x + 0.3f, f.y + 1.8f, f.z + 0.3f}}))
                            continue;
                        const V3 eye = f + V3{0, 1.65f, 0};
                        for (int k = 0; k < 16; ++k) {
                            const float a = k * 3.14159265f / 8.f;
                            const V3    d{std::sin(a), 0, -std::cos(a)};
                            SRayHit     w;
                            if (!col.raycast(eye, d, 9.f, w) || w.t < 3.f || std::abs(w.normal.y) > 0.2f)
                                continue;
                            const V3 n = normalize(V3{w.normal.x, 0, w.normal.z});
                            if (dot(n, d) > -0.85f)
                                continue; // only walls seen head on
                            const V3 right = normalize(cross(up, n));
                            const V3 c     = eye + d * w.t + n * 0.01f;
                            bool     ok    = true;
                            for (int iy = 0; iy < 5 && ok; ++iy)
                                for (int ix = 0; ix < 9 && ok; ++ix) {
                                    const V3 q = c + right * (W * (ix / 8.f - 0.5f)) + up * (H * (iy / 4.f - 0.5f));
                                    SRayHit  r;
                                    if (!col.raycast(q + n * 0.3f, -n, 0.6f, r) || std::abs(r.t - 0.31f) > 0.08f || dot(r.normal, n) < 0.9f)
                                        ok = false;
                                    else if (col.raycast(q + n * 0.03f, n, 1.5f, r))
                                        ok = false;
                                    else {
                                        const V3    to  = q + n * 0.02f - eye;
                                        const float len = length(to);
                                        if (col.raycast(eye, to / len, len - 0.03f, r))
                                            ok = false;
                                    }
                                }
                            if (!ok)
                                continue;
                            SRayHit     bk;
                            const float back = col.raycast(eye, -d, 30.f, bk) ? bk.t : 30.f;
                            const float e    = autoExposure(eye, d);
                            cands.push_back({f, c, n, w.t, back, e, 0});
                        }
                    }
                }
            for (auto& c : cands)
                c.score = std::min(c.back, 15.f) / 15.f + (c.exposure < 1.3f ? 1.f : 0.f) - std::abs(c.dist - 4.5f) * 0.1f;
            std::ranges::sort(cands, [](const SCand& a, const SCand& b) { return a.score > b.score; });
            std::vector<SCand> out;
            for (const auto& c : cands) {
                if (std::ranges::any_of(out, [&](const SCand& o) { return length(o.center - c.center) < 6.f; }))
                    continue;
                out.push_back(c);
                if (out.size() >= 24)
                    break;
            }
            for (const auto& c : out)
                fprintf(stderr, "wall %.2f %.2f %.2f n %.2f %.2f feet %.2f %.2f %.2f dist %.1f back %.1f exp %.2f score %.2f yaw %.0f\n", c.center.x, c.center.y,
                        c.center.z, c.normal.x, c.normal.z, c.feet.x, c.feet.y, c.feet.z, c.dist, c.back, c.exposure, c.score,
                        std::atan2(-c.normal.x, c.normal.z) * DEG);
            fprintf(stderr, "%zu candidate walls\n", cands.size());
        } else if (a == "--third") {
            firstPerson = false;
        } else if (a == "--hide") { // name: stop drawing the map's materials whose name has it (debugging)
            need(i, 1);
            const std::string what = argv[++i];
            int               n    = 0;
            if (world.model)
                for (auto& b : world.model->batches)
                    if (world.model->materials[b.material].name.find(what) != std::string::npos) {
                        b.render = false;
                        ++n;
                    }
            fprintf(stderr, "hid %d batches of %s\n", n, what.c_str());
        } else if (a == "--hide-avatar") { // name: stop drawing the avatar's materials whose name has it (to see what's behind)
            need(i, 1);
            const std::string what = argv[++i];
            int               n    = 0;
            if (model)
                for (auto& b : model->batches)
                    if (model->materials[b.material].name.find(what) != std::string::npos) {
                        b.count = 0;
                        ++n;
                    }
            fprintf(stderr, "hid %d of the avatar's batches of %s\n", n, what.c_str());
        } else if (a == "--show") { // name: draw them again
            need(i, 1);
            const std::string what = argv[++i];
            if (world.model)
                for (auto& b : world.model->batches)
                    if (world.model->materials[b.material].name.find(what) != std::string::npos)
                        b.render = true;
        } else if (a == "--plain") {
            plainTextures = true;
        } else if (a == "--no-dual") { // glass without blending's second source, as where there's none (before --map)
            renderer.dualSource = false;
        } else if (a == "--glinfo") {
            const char* ext = (const char*)glGetString(GL_EXTENSIONS);
            fprintf(stderr, "GL_VERSION %s\nGL_RENDERER %s\n", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
            for (const char* e : {"GL_EXT_texture_compression_s3tc", "GL_EXT_texture_compression_s3tc_srgb", "GL_NV_sRGB_formats", "GL_EXT_texture_compression_rgtc", "GL_EXT_texture_compression_bptc",
                                  "GL_KHR_texture_compression_astc_ldr", "GL_EXT_texture_filter_anisotropic", "GL_EXT_color_buffer_float", "GL_EXT_color_buffer_half_float",
                                  "GL_EXT_blend_func_extended", "GL_EXT_shader_framebuffer_fetch", "GL_EXT_shader_framebuffer_fetch_non_coherent", "GL_ARM_shader_framebuffer_fetch",
                                  "GL_NV_shader_framebuffer_fetch", "GL_KHR_blend_equation_advanced"})
                fprintf(stderr, "%s %s\n", e, ext && std::strstr(ext, e) ? "yes" : "no");
        } else if (a == "--bench") {
            need(i, 1);
            bench = atoi(argv[++i]);
        } else if (a == "--audio") { // file.wav [start seconds]: lip sync from it, mono or its channels mixed
            need(i, 1);
            const std::string file  = argv[++i];
            const float       start = i + 1 < argc && argv[i + 1][0] != '-' ? (float)atof(argv[++i]) : 0.f;
            std::ifstream     in(file, std::ios::binary);
            std::vector<char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto              u16 = [&](size_t o) { return (uint32_t)(uint8_t)d[o] | (uint32_t)(uint8_t)d[o + 1] << 8; };
            auto              u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
            int               fmt = 0, ch = 0, bits = 0;
            audio.clear();
            for (size_t o = 12; d.size() >= 12 && o + 8 <= d.size();) {
                const std::string id(d.data() + o, 4);
                const size_t      len = u32(o + 4), body = o + 8;
                if (id == "fmt " && body + 16 <= d.size()) {
                    fmt       = (int)u16(body);
                    ch        = (int)u16(body + 2);
                    audioRate = (int)u32(body + 4);
                    bits      = (int)u16(body + 14);
                    if (fmt == 0xFFFE && body + 26 <= d.size())
                        fmt = (int)u16(body + 24); // WAVE_FORMAT_EXTENSIBLE: its sub format
                } else if (id == "data" && ch > 0 && bits > 0) {
                    const size_t bytes = (size_t)bits / 8, frames = std::min(len, d.size() - body) / (bytes * ch);
                    for (size_t k = 0; k < frames; ++k) {
                        float sum = 0;
                        for (int c = 0; c < ch; ++c) {
                            const char* q = d.data() + body + (k * ch + c) * bytes;
                            float       v = 0;
                            if (fmt == 3 && bits == 32)
                                std::memcpy(&v, q, 4);
                            else if (fmt == 1 && bits == 16)
                                v = (int16_t)((uint8_t)q[0] | (uint8_t)q[1] << 8) / 32768.f;
                            else if (fmt == 1 && bits == 24)
                                v = (float)((int32_t)((uint32_t)(uint8_t)q[0] << 8 | (uint32_t)(uint8_t)q[1] << 16 | (uint32_t)(uint8_t)q[2] << 24) >> 8) / 8388608.f;
                            else if (fmt == 1 && bits == 32)
                                v = (float)((int32_t)((uint32_t)(uint8_t)q[0] | (uint32_t)(uint8_t)q[1] << 8 | (uint32_t)(uint8_t)q[2] << 16 | (uint32_t)(uint8_t)q[3] << 24) / 2147483648.0);
                            sum += v;
                        }
                        audio.push_back(sum / ch);
                    }
                }
                o = body + len + (len & 1);
            }
            audioAt = std::min(audio.size(), (size_t)(start * audioRate));
            lip.reset();
            fprintf(stderr, "audio %s: %.2f s at %d Hz%s\n", file.c_str(), audioRate ? (double)audio.size() / audioRate : 0.0, audioRate,
                    audio.empty() ? " (no samples: PCM 16/24/32-bit or 32-bit float WAV)" : "");
        } else if (a == "--lipsync-gain") { // dB|auto: lip sync's gain, for what follows (the plugin's lipsync_gain)
            need(i, 1);
            const std::string v = argv[++i];
            lipGain             = v == "auto" ? std::nullopt : std::optional<float>((float)atof(v.c_str()));
            lip.setGain(lipGain);
        } else if (a == "--lipsync-trace") { // the rest of --audio's file through lip sync, a line a window
            CLipSync     tr;
            tr.setGain(lipGain);
            const size_t from = audioAt;
            for (size_t k = audioAt; k < audio.size(); k += 32) {
                const size_t seen = tr.windows();
                tr.feed(audio.data() + k, std::min<size_t>(32, audio.size() - k), audioRate);
                if (tr.windows() == seen)
                    continue;
                const auto& w = tr.last();
                const auto& v = tr.visemes();
                std::string shape, out;
                for (int s = 0; s < VISEME_COUNT; ++s) {
                    shape += std::format(" {:.2f}", w.shape[s]);
                    out += std::format(" {:.2f}", v[s]);
                }
                // (the marks after the consonant: the fields before it keep their places, the mouth's nine stay last)
                fprintf(stderr, "window %.3f s: level %.1f gain %.1f crossings %.3f periodic %.3f %s F1 %.0f F2 %.0f bands %.3f %.3f %.3f under %.1f consonant %s marks %.1f %.1f amp %.1f shape%s out%s\n",
                        (double)(k - from) / audioRate, w.level, w.gain, w.crossings, w.periodic, w.voiced ? "voiced" : "unvoiced", w.f1, w.f2, w.mid, w.high, w.low,
                        w.under, w.consonant >= 0 ? VISEME_NAMES[w.consonant] : "-", w.lo, w.hi, tr.gain(), shape.c_str(), out.c_str());
            }
        } else if (a == "--badge") { // text [scale]: the plugin's corner badge (lip sync's "lip sync: listening"), at a monitor's scale
            need(i, 1);
            badgeText = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-')
                badgeScale = (float)atof(argv[++i]);
        } else if (a == "--visemes") { // what lip sync heard last: aa ih ou ee oh, the level, the formants
            const auto& v = lip.visemes();
            fprintf(stderr, "visemes aa %.2f ih %.2f ou %.2f ee %.2f oh %.2f pp %.2f ff %.2f ss %.2f ch %.2f, level %.1f dBFS, F1 %.0f F2 %.0f Hz, gain %.1f dB\n", v[0], v[1],
                    v[2], v[3], v[4], v[5], v[6], v[7], v[8], lip.level(), lip.f1(), lip.f2(), lip.gain());
        } else if (a == "--outlines") { // 1|0: the avatar's toon outlines
            need(i, 1);
            outlines = atoi(argv[++i]) != 0;
        } else if (a == "--autoexp") { // 1|0: set the exposure like the plugin does, at each --out
            need(i, 1);
            autoExp = atoi(argv[++i]) != 0;
        } else if (a == "--out") {
            need(i, 1);
            const std::string path = argv[++i];
            if (!outTex || texW != W || texH != H) {
                if (outTex)
                    glDeleteTextures(1, &outTex);
                glGenTextures(1, &outTex);
                glBindTexture(GL_TEXTURE_2D, outTex);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
                texW = W;
                texH = H;
            }
            const float h      = model ? model->height : 1.7f;
            V3          target = feet + shift + V3{0, targetY >= 0 ? targetY : h * 0.55f, 0};
            if (model && !aimName.empty()) {
                int node = -1;
                static const std::pair<const char*, int> HUMAN[] = {{"lefthand", HB_L_HAND}, {"righthand", HB_R_HAND}, {"head", HB_HEAD}, {"hips", HB_HIPS},
                                                                    {"leftfoot", HB_L_FOOT}, {"rightfoot", HB_R_FOOT}, {"chest", HB_CHEST}};
                for (auto [n, b] : HUMAN)
                    if (aimName == n)
                        node = model->human[b];
                for (size_t k = 0; k < model->nodes.size() && node < 0; ++k)
                    if (model->nodes[k].name == aimName)
                        node = (int)k;
                const auto& J = anim.joints();
                for (size_t j = 0; j < model->joints.size(); ++j)
                    if (model->joints[j].node == node) {
                        M4 skin = M4::identity();
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 4; ++c)
                                skin.m[c * 4 + r] = J[j * 12 + r * 4 + c];
                        const M4 ib   = model->joints[j].inverseBind.inverse();
                        const V3 bind{ib.m[12], ib.m[13], ib.m[14]};
                        const M4 toWorld = M4::trs(feet + V3{0, anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1}) * model->fix;
                        target = toWorld.point(skin.point(bind)) + shift;
                        break;
                    }
            }
            float facing = camWorld ? 0.f : bodyYaw;
            if (const int chest = model ? model->human[model->human[HB_CHEST] >= 0 ? HB_CHEST : HB_SPINE] : -1; camChest && chest >= 0) {
                M4 rest = M4::identity();
                for (int k = chest; k >= 0; k = model->nodes[k].parent)
                    rest = model->nodes[k].rest.matrix() * rest;
                const M4 toWorld = M4::trs(feet, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1}) * model->fix;
                const V3 f       = toWorld.dir(anim.globals()[chest].dir(rest.inverse().dir(model->forward)));
                if (std::hypot(f.x, f.z) > 1e-3f)
                    facing = std::atan2(f.x, -f.z);
            }
            const V3    toCam  = forwardFrom(facing + orbit, rad(pitch));
            V3          eye    = target + toCam * dist;
            if (firstPerson) {
                eye    = eyeSet ? eyeAt : fpBody && model ? mo.fp.eye : feet + V3{0, 1.65f, 0};
                target = eye + forwardFrom(rad(camYaw), rad(camPitch));
            }
            if (autoExp) {
                exposure = autoExposure(eye, normalize(target - eye));
                fprintf(stderr, "exposure %.2f\n", exposure);
            }
            const float farPlane = world.model ? std::max(200.f, length(world.bounds.size()) * 1.5f) : 200.f;

            SFrameParams f;
            f.width     = W;
            f.height    = H;
            f.view      = M4::lookAt(eye, target, {0, 1, 0});
            f.proj      = M4::perspective(rad(fov), (float)W / H, 0.05f, farPlane);
            f.eye       = eye;
            f.time      = time;
            f.panels    = &panels;
            f.crosshair = false;
            f.hudAlpha  = 0;
            f.exposure  = exposure;
            menu.update(menuDt, W, H, 1);
            f.menu = menu.hud();
            if (!badgeText.empty()) { // as the plugin shows it while lip sync listens
                drawBadge(badgePixels, badgeW, badgeH, badgeText, badgeScale);
                const float margin = 12.f * badgeScale;
                f.badge = {.pixels = &badgePixels, .w = badgeW, .h = badgeH, .serial = ++badgeSerial, .x = W - margin - badgeW / 2.f, .y = margin + badgeH / 2.f};
                fprintf(stderr, "badge \"%s\": %d x %d px at scale %.2f\n", badgeText.c_str(), badgeW, badgeH, badgeScale);
            }
            if (menu.visible()) {
                std::string items;
                for (const auto& it : menu.page().items)
                    items += std::format(" [{}{}{}{}]", it.label, it.hint.empty() ? "" : ": " + it.hint, it.on ? " ON" : "", it.disabled ? " off" : "");
                fprintf(stderr, "menu %s \"%s\" highlight %d, %zu px:%s\n", menu.path().c_str(), menu.page().title.c_str(), menu.highlighted(), (size_t)f.menu.w, items.c_str());
            }
            if (model) {
                f.avatar.model     = model;
                f.avatar.joints    = &anim.joints();
                f.avatar.morphs    = &anim.morphWeights();
                f.avatar.materials = anim.materials();
                f.avatar.shown     = &anim.partsShown();
                f.avatar.batchMaterials = anim.batchMaterials();
                f.avatar.transform = M4::trs(feet + V3{0, anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -bodyYaw), {1, 1, 1});
                f.avatar.visible   = true;
                f.avatar.firstPerson = firstPerson && fpBody && !eyeSet;
                f.avatar.outlines  = outlines;
                f.avatar.sky       = sky;
                f.avatar.bounce    = bounce;
            }
            renderer.render(f, outTex);
            if (bench > 0) {
                // the same frame again and again: how long one takes on the GPU
                glFinish();
                const auto t0 = std::chrono::steady_clock::now();
                for (int k = 0; k < bench; ++k)
                    renderer.render(f, outTex);
                glFinish();
                fprintf(stderr, "bench: %.2f ms a frame (%d frames, %dx%d)\n",
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / bench, bench, W, H);
            }

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0);
            std::vector<uint8_t> px((size_t)W * H * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            if (const GLenum e = glGetError(); e)
                fprintf(stderr, "GL error 0x%x\n", e);
            writePNG(path, W, H, px);
            fprintf(stderr, "wrote %s (%s)\n", path.c_str(), model ? anim.playing().c_str() : "no avatar");
        } else {
            fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    return 0;
}
