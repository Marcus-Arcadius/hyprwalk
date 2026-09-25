// hypr3d: walk around your Hyprland desktop in first person.
//
// While 3D mode is on, a custom pass element covers the whole monitor. It
// draws a small procedural world plus every window/layer/popup as a textured
// panel hanging on its north wall, and hands the result back to Hyprland as a
// plain texture. Input is taken over: the mouse turns the camera, WASD walks,
// and clicks/scrolls go to whatever surface the crosshair points at. With an
// avatar loaded (plugin:hypr3d:avatar), V switches to a third person view of it,
// and Tab opens the Action Menu: its emotes, expressions, gestures and outfit.

#include "control.hpp"
#include "globals.hpp"
#include "lipsync.hpp"
#include "mic.hpp"
#include "gltf.hpp"
#include "map.hpp"
#include "math3d.hpp"
#include "menu.hpp"
#include "panels.hpp"
#include "renderer.hpp"
#include "world.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/errorOverlay/Overlay.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/notification/NotificationOverlay.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <array>
#include <dlfcn.h>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <numbers>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace h3d {
    void log(const std::string& s) {
        Log::logger->log(Log::INFO, "[hypr3d] {}", s);
    }

    void notify(const std::string& s, bool error) {
        if (error)
            Log::logger->log(Log::ERR, "[hypr3d] {}", s); // INFO is hidden unless debug logs are on
        else
            log(s);
        if (PHANDLE)
            HyprlandAPI::addNotification(PHANDLE, "[hypr3d] " + s, error ? CHyprColor{1.0, 0.35, 0.35, 1.0} : CHyprColor{0.45, 0.8, 1.0, 1.0}, error ? 8000 : 4000);
    }
}

using namespace h3d;

namespace {
    constexpr float F_PI            = std::numbers::pi_v<float>;
    constexpr float FOV_Y         = 70.f * F_PI / 180.f;
    constexpr float ENTER_TIME    = 0.8f;
    constexpr float EXIT_TIME     = 0.6f;

    constexpr float RADIUS        = 0.3f;
    constexpr float HEIGHT        = 1.8f;
    constexpr float HEIGHT_CROUCH = 1.2f;
    constexpr float EYE           = 1.65f;
    constexpr float EYE_CROUCH    = 1.1f;
    constexpr float GRAVITY       = 20.f;
    constexpr float JUMP_SPEED    = 6.3f;
    constexpr float WALK_SPEED    = 4.5f;
    constexpr float SPRINT_SPEED  = 7.f;
    constexpr float CROUCH_SPEED  = 2.f;
    constexpr float FLY_SPEED     = 8.f;
    constexpr float STEP_HEIGHT   = 0.5f;

    // evdev key codes
    enum : uint32_t {
        K_ESC    = 1,
        K_1      = 2, // .. K_8 = 9
        K_8      = 9,
        K_BACKSPACE = 14,
        K_TAB    = 15,
        K_W      = 17,
        K_E      = 18,
        K_R      = 19,
        K_ENTER  = 28,
        K_LCTRL  = 29,
        K_A      = 30,
        K_S      = 31,
        K_D      = 32,
        K_F      = 33,
        K_G      = 34,
        K_LSHIFT = 42,
        K_X      = 45,
        K_C      = 46,
        K_V      = 47,
        K_RSHIFT = 54,
        K_LALT   = 56,
        K_SPACE  = 57,
        K_F1     = 59, // .. K_F8 = 66
        K_F8     = 66,
        K_KPENTER = 96,
        K_RCTRL  = 97,
        K_RALT   = 100,
        K_UP     = 103,
        K_LEFT   = 105,
        K_RIGHT  = 106,
        K_DOWN   = 108,
        K_LMETA  = 125,
        K_RMETA  = 126,
    };

    constexpr uint32_t BTN_LEFT_   = 0x110;
    constexpr uint32_t BTN_RIGHT_  = 0x111;
    constexpr uint32_t BTN_MIDDLE_ = 0x112;

    uint32_t           nowMs() {
        return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    float wrapAngle(float a) {
        a = std::fmod(a + F_PI, 2.f * F_PI);
        if (a < 0)
            a += 2.f * F_PI;
        return a - F_PI;
    }

    V3 forwardFrom(float yaw, float pitch) {
        return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    }

}

constexpr const char* C3D_PASS_NAME = "CHypr3DElement";

enum eMode : uint8_t {
    MODE_OFF = 0,
    MODE_ENTERING,
    MODE_ACTIVE,
    MODE_EXITING,
};

struct SCamera {
    V3    eye;
    float yaw = 0, pitch = 0;
};

// a window taken off the desktop wall and put somewhere in the world
struct SPlacement {
    PHLWINDOWREF window;
    V3           center, targetCenter; // world position of the window's middle
    Quat         rot, targetRot;       // local x = right, y = up, z = the side the content faces
    float        scale = 1, targetScale = 1; // meters per logical px
    bool         returning = false;    // flying back to the wall, dropped once it's there
    bool         settled   = false;    // current pose caught up with the target
};

namespace {
    // plugin config, see PLUGIN_INIT
    SP<Config::Values::CFloatValue> g_cfgSpacing;
    SP<Config::Values::CBoolValue>  g_cfgWallpaper;
    SP<Config::Values::CStringValue> g_cfgMap;
    SP<Config::Values::CFloatValue>  g_cfgMapScale;
    SP<Config::Values::CStringValue> g_cfgAvatar;
    SP<Config::Values::CFloatValue>  g_cfgAvatarHeight;
    SP<Config::Values::CBoolValue>   g_cfgAvatarPhysics;
    SP<Config::Values::CBoolValue>   g_cfgLipSync;
    SP<Config::Values::CStringValue> g_cfgAvatarEmotes;

    // a file named in the config, "" when unset
    std::string configuredPath(const SP<Config::Values::CStringValue>& value) {
        std::string v = value ? value->value() : "";
        if (v == "[[EMPTY]]")
            v.clear();
        if (v.starts_with("~/"))
            if (const char* home = getenv("HOME"))
                v = home + v.substr(1);
        return v;
    }

    // a path as it's given: without quotes, ~ for the home folder
    std::string unquote(std::string v) {
        const size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t\n");
        v              = a == std::string::npos ? "" : v.substr(a, b - a + 1);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
            v = v.substr(1, v.size() - 2);
        if (v.starts_with("~/"))
            if (const char* home = getenv("HOME"))
                v = home + v.substr(1);
        return v;
    }

    // the emote files and folders in the config, separated by commas
    std::vector<std::string> configuredEmotes() {
        std::string v = g_cfgAvatarEmotes ? g_cfgAvatarEmotes->value() : "";
        if (v == "[[EMPTY]]")
            v.clear();
        std::vector<std::string> out;
        for (size_t at = 0; at <= v.size();) {
            const size_t comma = std::min(v.find(',', at), v.size());
            if (std::string f = unquote(v.substr(at, comma - at)); !f.empty())
                out.push_back(std::move(f));
            at = comma + 1;
        }
        return out;
    }

    // the configured map, "" for the built-in courtyard
    std::string configuredMap() {
        return configuredPath(g_cfgMap);
    }

    float layerSpacing() {
        const float v = g_cfgSpacing ? g_cfgSpacing->value() : 0.f;
        return v > 0.f ? std::min(v, 0.5f) : 0.02f;
    }
}

class CDesktop3D {
  public:
    void                          init();
    void                          shutdown();

    bool                          enter(PHLMONITOR mon = nullptr);
    void                          exit(bool immediate = false);
    void                          toggle();
    void                          setTyping(bool on);

    std::string                   hyprctl(const std::string& request);
    // the Action Menu: open [page], close, toggle, back, pick [n], move dx dy, scroll n; none = what it shows
    std::string                   menuCommand(const std::vector<std::string>& args);
    std::string                   menuDispatch(const std::string& arg); // nothing = toggle, a page = open it, else a command

    // hooks, return true when the event was eaten
    bool                          onRelativeMotion(const Vector2D& delta);
    bool                          onAbsoluteMotion(const Vector2D& abs);
    bool                          cursorHidden() const {
        return m_mode != MODE_OFF;
    }

    std::vector<UP<IPassElement>> drawFrame();
    PHLMONITOR                    monitor() const {
        return m_monitor.lock();
    }

  private:
    eMode         m_mode = MODE_OFF;
    PHLMONITORREF m_monitor;
    float         m_t    = 0; // 0 = flat 2D, 1 = fully in 3D
    float         m_time = 0;
    std::chrono::steady_clock::time_point m_lastUpdate;
    float                                 m_fps = 0;
    uint64_t                              m_frames = 0, m_framesAtLastFps = 0;
    std::chrono::steady_clock::time_point m_lastFps;
    float                                 m_minDt = 1.f;

    // world
    SWorld         m_world;
    SScreenMapping m_screen; // where the desktop hangs
    Vector2D       m_worldFor{-1, -1};
    CRenderer      m_renderer;
    bool           m_rendererDirty = true;
    bool           m_rendererFailed = false;
    SP<Render::ITexture> m_outTex;
    Vector2D             m_outSize;

    // loaded maps
    CMapLoader           m_mapLoader;
    wl_event_source*     m_mapSource = nullptr;
    std::string          m_mapPath;          // shown (or being loaded), "" = the courtyard
    std::string          m_mapConfigured;    // last value seen in the config
    float                m_mapScale = 0;     // what the loaded map was scaled by

    // the avatar: the player's body, seen in third person (and its shadow in first)
    CAvatarLoader                 m_avatarLoader;
    wl_event_source*              m_avatarSource = nullptr;
    std::string                   m_avatarPath;       // shown (or being loaded), "" = none
    std::string                   m_avatarConfigured; // last value seen in the config
    int                           m_physicsConfigured = -1; // the same for avatar_physics (hyprctl can change it in between)
    float                         m_avatarHeight = 0; // asked for, 0 = as it comes
    std::shared_ptr<SAvatarModel> m_avatar;
    CAvatarAnimator               m_anim;
    CAvatarControl                m_ctl{m_anim}; // what hyprctl and the Action Menu do to it (control.cpp), and what was set by hand
    // lip sync: asked for (the config, hyprctl, the menu); the microphone listens only while that's so and the 3D
    // desktop is up with an avatar, and a badge says so
    bool                          m_lipsync = false;
    int                           m_lipsyncConfigured = -1; // the config's, last seen
    CMicrophone                   m_mic;
    CLipSync                      m_lip;
    std::vector<float>            m_micSamples;
    std::vector<uint32_t>         m_badge;
    int                           m_badgeW = 0, m_badgeH = 0;
    float                         m_badgeScale = 0;
    uint64_t                      m_badgeSerial = 0;
    // emotes from files: the config's, and those hyprctl adds (made again for each avatar that loads)
    CEmoteLoader                  m_emoteLoader;
    wl_event_source*              m_emoteSource = nullptr;
    std::vector<std::string>      m_emoteFiles;             // added with hyprctl, absolute
    std::vector<std::string>      m_emoteLoading;           // what m_emoteLoader makes, till it's done
    std::string                   m_emotesConfigured = "\n"; // last value seen in the config ("\n" = none seen)
    std::string                   m_emotePlay;              // a file whose first emote plays when it's made
    int                           m_emotePlayLoop = -1;
    float                         m_bodyYaw = 0;      // where the body faces
    bool                          m_bodyTurning = false;
    float                         m_lookYaw = 0, m_lookPitch = 0; // where its head turns, relative to the body
    // light around it, measured the way the map bakes its own
    struct {
        std::array<float, 32> sky{}, bounce{};
        int                   next = 0;
        bool                  full = false;
        float                 skyAvg = 1, bounceAvg = 0;
    } m_avatarLight;

    // third person: the camera hangs behind the avatar's shoulder
    bool  m_thirdPerson = false;
    float m_camDist     = 2.6f; // wanted boom length (wheel)
    float m_camSide     = 0.4f; // over the right shoulder
    float m_camBoom     = 0;    // actual, after walls pulled it in

    // eyes adjusting to the dark: a few rays a frame measure the light around the camera
    float                 m_exposure = 1;
    float                 m_lightAvg = 0;
    std::array<float, EXPOSURE_SAMPLES> m_lightProbe{};
    int                   m_lightNext = 0;
    bool                  m_lightFull = false; // false: measure everything again at once (after a jump)

    // player
    V3    m_feet;
    V3    m_vel;
    float m_yaw = 0, m_pitch = 0;
    float m_eyeHeight = EYE;
    bool  m_onGround  = true;
    bool  m_crouched  = false;
    bool  m_fly       = false;
    float m_sens      = 0.0022f; // radians per mouse count
    float m_scriptWalk = 0;      // hyprctl "walk": seconds left
    V3    m_scriptDir;
    bool  m_scriptJump = false;
    SCamera m_exitFrom;

    // input
    std::array<bool, 256>        m_keys{};
    std::unordered_set<uint32_t> m_consumed;    // key presses we ate, so their releases get eaten too
    std::unordered_set<uint32_t> m_sentButtons; // buttons forwarded to clients and not released yet
    bool                         m_typing = false;
    Vector2D                     m_look;
    Vector2D                     m_lastAbs{-1, -1};
    std::array<double, 2>        m_scrollAcc{}; // unsent fractions of a wheel notch, in 1/120ths, per axis

    // the Action Menu (Tab), like VRChat's: while it's open the mouse moves its cursor, not the camera
    CActionMenu m_menu{[this](const std::string& id) {
                           return actionPage(id, {m_avatar.get(), &m_anim, m_avatarLoader.busy(), m_thirdPerson, m_fly, m_lipsync, CMicrophone::available()});
                       },
                       [this](const SMenuItem& it, float v, float v2) { m_ctl.dial(it, v, v2); }}; // a slider's dial (a stick: both)
    float       m_menuWheel = 0; // a fraction of a notch

    // aiming
    std::vector<SPanel>        m_panels;
    int                        m_aimed = -1;
    Vector2D                   m_aimPanelLocal;
    WP<CWLSurfaceResource>     m_aimSurface;
    Vector2D                   m_aimLocal;
    Vector2D                   m_lastSentLocal{-1, -1};
    struct {
        uintptr_t              panel = 0;
        WP<CWLSurfaceResource> surface;
        Vector2D               offset; // hit-root local - surface local at press time
    } m_drag;

    SCamera                   m_camera;
    M4                        m_view, m_proj;
    float                     m_depthMul = 0;

    // windows out in the world, by window
    std::unordered_map<uintptr_t, SPlacement> m_placements;
    struct {
        uintptr_t  key = 0;      // window being carried, 0 = none
        float      dist = 2;     // how far in front of the eye
        float      scaleMul = 1; // ctrl+wheel resizes it
        bool       hadBefore = false;
        SPlacement before;       // to put it back on escape
    } m_hold;

    bool                      m_prevDSBlocked = false;
    UP<SEventLoopDoLaterLock> m_restoreLater;

    // Hyprland glue
    std::vector<CHyprSignalListener> m_listeners;
    wl_event_source*                 m_configTimer = nullptr; // looks at the config values now and then
    SP<SHyprCtlCommand>              m_ctlCommand;
    CFunctionHook*                   m_hookMoved  = nullptr;
    CFunctionHook*                   m_hookWarp   = nullptr;
    CFunctionHook*                   m_hookCursor = nullptr;

    void                             buildWorldFor(const Vector2D& logical);
    void                             checkMapConfig();
    std::string                      requestMap(const std::string& path, float scale);
    void                             applyMap(SMapResult&& res);
    void                             checkAvatarConfig();
    std::string                      requestAvatar(const std::string& path, float height);
    void                             applyAvatar(SAvatarResult&& res);
    std::string                      avatarStatus() const;
    std::string                      loadEmoteFile(const std::string& file, int loop); // hyprctl's: made, then played
    std::string                      setLipSync(bool on);
    void                             lipSync(); // every frame: the microphone on or off, what it heard to the mouth
    std::string                      lipSyncStatus() const;
    std::vector<std::string>         emoteFiles() const; // the config's, then those added
    void                             loadEmoteFiles(std::vector<std::string> files);
    void                             applyEmotes(SEmoteResult&& res);
    std::string                      menuAction(const SMenuItem& item);
    void                             menuPick(const std::optional<SMenuItem>& item);
    std::string                      setView(bool third);
    void                             animateAvatar(float dt);
    void                             measureAvatarLight();
    M4                               avatarTransform() const;
    void                             dropPlacements();
    std::string                      placeDesktop(float height);
    SCamera                          flatCamera() const;
    SCamera                          playerCamera() const;
    SCamera                          viewCamera(float dt); // first or third person
    void                             update();
    void                             simulate(float dt);
    void                             moveAxis(int axis, float d, bool& blocked);
    bool                             overlaps(const V3& feet, float height) const;
    void                             layoutPanels(float e);
    void                             updatePlacements(float dt);
    void                             grab();
    void                             place();
    void                             cancelHold();
    void                             returnToWall(uintptr_t key);
    SPlacement                       layoutPlacement(const SPanel& p) const;
    void                             aim();
    void                             updatePointer();
    void                             exitNow();
    void                             restore();
    void                             resetPlayer();
    void                             adaptExposure(float dt);
    void                             onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info);
    void                             onButton(uint32_t timeMs, uint32_t button, bool pressed, Event::SCallbackInfo* info);
    void                             onAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info);
    std::string                      status();

    friend class C3DElement;
};

static std::unique_ptr<CDesktop3D> g_p3D;

// ------------------------------------------------------------------ element

class C3DElement : public IPassElement {
  public:
    std::vector<UP<IPassElement>> draw() override {
        if (!g_p3D)
            return {};
        return g_p3D->drawFrame();
    }
    bool needsLiveBlur() override {
        return false;
    }
    bool needsPrecomputeBlur() override {
        return false;
    }
    const char* passName() override {
        return C3D_PASS_NAME;
    }
    ePassElementType type() override {
        return EK_CUSTOM;
    }
    bool undiscardable() override {
        return true;
    }
    std::optional<CBox> boundingBox() override {
        const auto mon = g_p3D ? g_p3D->monitor() : nullptr;
        if (!mon)
            return std::nullopt;
        return CBox{{0, 0}, mon->m_size};
    }
    CRegion opaqueRegion() override {
        const auto mon = g_p3D ? g_p3D->monitor() : nullptr;
        if (!mon)
            return {};
        return CRegion{CBox{{0, 0}, mon->m_size}};
    }
};

// -------------------------------------------------------------------- hooks

namespace {
    using FnMouseMoved = void (*)(void*, IPointer::SMotionEvent);
    using FnMouseWarp  = void (*)(void*, IPointer::SMotionAbsoluteEvent);
    using FnEnsure     = void (*)(void*);

    CFunctionHook* g_moved  = nullptr;
    CFunctionHook* g_warp   = nullptr;
    CFunctionHook* g_cursor = nullptr;

    void           hkMouseMoved(void* self, IPointer::SMotionEvent e) {
        if (g_p3D && g_p3D->onRelativeMotion(e.unaccel != Vector2D{} ? e.unaccel : e.delta))
            return;
        ((FnMouseMoved)g_moved->m_original)(self, e);
    }

    void hkMouseWarp(void* self, IPointer::SMotionAbsoluteEvent e) {
        if (g_p3D && g_p3D->onAbsoluteMotion(e.absolute))
            return;
        ((FnMouseWarp)g_warp->m_original)(self, e);
    }

    void hkEnsureCursor(void* self) {
        if (g_p3D && g_p3D->cursorHidden()) {
            g_pHyprRenderer->setCursorHidden(true);
            return;
        }
        ((FnEnsure)g_cursor->m_original)(self);
    }

    CFunctionHook* hookByName(const std::string& name, const std::string& needle, void* dst) {
        for (const auto& fn : HyprlandAPI::findFunctionsByName(PHANDLE, name)) {
            if (!fn.demangled.contains(needle))
                continue;
            auto* hook = HyprlandAPI::createFunctionHook(PHANDLE, fn.address, dst);
            if (hook && hook->hook()) {
                logf("hooked {}", fn.demangled);
                return hook;
            }
            if (hook)
                HyprlandAPI::removeFunctionHook(PHANDLE, hook);
        }
        logf("could not hook {}", needle);
        return nullptr;
    }
}

// --------------------------------------------------------------- lifecycle

void CDesktop3D::init() {
    m_hookMoved  = g_moved  = hookByName("onMouseMoved", "CInputManager::onMouseMoved(", (void*)&hkMouseMoved);
    m_hookWarp   = g_warp   = hookByName("onMouseWarp", "CInputManager::onMouseWarp(", (void*)&hkMouseWarp);
    m_hookCursor = g_cursor = hookByName("ensureCursorRenderingMode", "HyprRenderer::ensureCursorRenderingMode(", (void*)&hkEnsureCursor);

    if (!m_hookMoved)
        notify("couldn't hook mouse motion: mouse look won't work (arrow keys still do)", true);

    auto& ev = Event::bus()->m_events;

    m_listeners.emplace_back(ev.render.pre.listen([this](const PHLMONITOR& mon) {
        if (m_mode == MODE_OFF || mon != m_monitor.lock())
            return;
        if (g_pSessionLockManager->isSessionLocked()) {
            exitNow();
            return;
        }
        g_pHyprRenderer->m_directScanoutBlocked = true;
        g_pHyprRenderer->setCursorHidden(true);
        // redraw everything, without damageMonitor(): that would also schedule
        // an extra frame outside of the display's pacing
        mon->m_damage.damageEntire();
    }));

    m_listeners.emplace_back(ev.render.stage.listen([this](eRenderStage stage) {
        if (stage != RENDER_LAST_MOMENT || m_mode == MODE_OFF)
            return;
        const auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!mon || mon != m_monitor.lock())
            return;
        if (g_pSessionLockManager->isSessionLocked()) {
            exitNow();
            return;
        }

        update();
        if (m_mode == MODE_OFF)
            return; // the exit animation just ended, show the real desktop this frame

        g_pHyprRenderer->m_renderPass.add(makeUnique<C3DElement>());
        // Hyprland's notifications (ours too) and its config error bar went into this frame before the 3D view, which
        // covers the monitor: again, over it (on the focused monitor, as Hyprland draws them)
        if (mon == Desktop::focusState()->monitor()) {
            Notification::overlay()->draw(mon);
            ErrorOverlay::overlay()->draw();
        }
        // keep frames coming: Hyprland schedules the next one right after this one is committed,
        // paced by the display (calling scheduleFrameForMonitor here would also queue an unpaced one)
        mon->m_pendingFrame = true;
    }));

    m_listeners.emplace_back(ev.monitor.removed.listen([this](const PHLMONITOR& mon) {
        if (m_mode != MODE_OFF && (!m_monitor.lock() || mon == m_monitor.lock()))
            exitNow();
    }));

    m_listeners.emplace_back(ev.input.keyboard.key.listen([this](const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) { onKey(e, info); }));

    m_listeners.emplace_back(ev.input.mouse.button.listen([this](const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        onButton(e.timeMs, e.button, e.state == WL_POINTER_BUTTON_STATE_PRESSED, &info);
    }));

    m_listeners.emplace_back(ev.input.mouse.axis.listen([this](const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) { onAxis(e, info); }));

    // no pointer refocusing, focus-follows-mouse or cursor warps while in 3D
    m_listeners.emplace_back(ev.input.mouse.move.listen([this](const Vector2D& pos, Event::SCallbackInfo& info) {
        if (m_mode != MODE_OFF)
            info.cancelled = true;
    }));

    m_mapSource = wl_event_loop_add_fd(
        g_pCompositor->m_wlEventLoop, m_mapLoader.fd(), WL_EVENT_READABLE,
        [](int, uint32_t, void* data) {
            auto* self = (CDesktop3D*)data;
            if (auto res = self->m_mapLoader.take())
                self->applyMap(std::move(*res));
            return 0;
        },
        this);

    m_avatarSource = wl_event_loop_add_fd(
        g_pCompositor->m_wlEventLoop, m_avatarLoader.fd(), WL_EVENT_READABLE,
        [](int, uint32_t, void* data) {
            auto* self = (CDesktop3D*)data;
            if (auto res = self->m_avatarLoader.take())
                self->applyAvatar(std::move(*res));
            return 0;
        },
        this);

    m_emoteSource = wl_event_loop_add_fd(
        g_pCompositor->m_wlEventLoop, m_emoteLoader.fd(), WL_EVENT_READABLE,
        [](int, uint32_t, void* data) {
            auto* self = (CDesktop3D*)data;
            if (auto res = self->m_emoteLoader.take())
                self->applyEmotes(std::move(*res));
            return 0;
        },
        this);

    m_listeners.emplace_back(ev.config.reloaded.listen([this] {
        checkMapConfig();
        checkAvatarConfig();
    }));
    // a value set at run time comes with no reload (hyprctl keyword, hl.config() through hyprctl eval): look again
    // every second
    m_configTimer = wl_event_loop_add_timer(
        g_pCompositor->m_wlEventLoop,
        [](void* data) {
            auto* self = (CDesktop3D*)data;
            self->checkMapConfig();
            self->checkAvatarConfig();
            wl_event_source_timer_update(self->m_configTimer, 1000);
            return 0;
        },
        this);
    wl_event_source_timer_update(m_configTimer, 1000);

    m_ctlCommand = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{
                                                                    .name  = "hypr3d",
                                                                    .exact = false,
                                                                    .fn    = [this](eHyprCtlOutputFormat, std::string request) { return hyprctl(request); },
                                                                });

    HyprlandAPI::addDispatcherV2(PHANDLE, "hypr3d:toggle", [this](std::string) {
        toggle();
        return SDispatchResult{};
    });

    // hypr3d:menu toggles the Action Menu, hypr3d:menu <page> opens that page, or a hyprctl hypr3d menu command
    HyprlandAPI::addDispatcherV2(PHANDLE, "hypr3d:menu", [this](std::string arg) {
        const std::string r = menuDispatch(arg);
        return r.starts_with("error: ") ? SDispatchResult{.success = false, .error = r.substr(7)} : SDispatchResult{};
    });

    checkMapConfig();
    checkAvatarConfig();
}

void CDesktop3D::shutdown() {
    // Hyprland only clears the render pass when the next frame begins; an
    // element left over from the last frame would then run our destructor
    // after this library is gone
    g_pHyprRenderer->m_renderPass.removeAllOfType(C3D_PASS_NAME);

    if (m_mode != MODE_OFF) {
        m_mode = MODE_OFF;
        restore();
    } else if (m_restoreLater) {
        m_restoreLater.reset();
        restore();
    }

    for (auto** h : {&m_hookMoved, &m_hookWarp, &m_hookCursor}) {
        if (*h)
            HyprlandAPI::removeFunctionHook(PHANDLE, *h);
        *h = nullptr;
    }
    g_moved = g_warp = g_cursor = nullptr;

    m_listeners.clear();
    if (m_configTimer)
        wl_event_source_remove(m_configTimer);
    m_configTimer = nullptr;
    if (m_mapSource)
        wl_event_source_remove(m_mapSource);
    m_mapSource = nullptr;
    m_mapLoader.cancel();
    if (m_avatarSource)
        wl_event_source_remove(m_avatarSource);
    m_avatarSource = nullptr;
    m_avatarLoader.cancel();
    if (m_emoteSource)
        wl_event_source_remove(m_emoteSource);
    m_emoteSource = nullptr;
    m_emoteLoader.cancel();
    if (m_ctlCommand)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, m_ctlCommand);
    m_ctlCommand.reset();
    HyprlandAPI::removeDispatcher(PHANDLE, "hypr3d:toggle");
    HyprlandAPI::removeDispatcher(PHANDLE, "hypr3d:menu");

    if (Render::GL::g_pHyprOpenGL) {
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        m_renderer.destroy();
    }
    m_outTex.reset();
    m_panels.clear();
}

// ------------------------------------------------------------- mode changes

void CDesktop3D::buildWorldFor(const Vector2D& logical) {
    if (logical == m_worldFor)
        return;

    if (m_world.model) {
        // a loaded map doesn't depend on the monitor, only the desktop's width does
        m_worldFor = logical;
        dropPlacements();
        m_screen.logicalSize = logical;
        m_screen.setAnchor(m_world.desktop);
        return;
    }

    SScreenSpec spec;
    spec.aspect = (float)(logical.x / logical.y);

    const auto start = std::chrono::steady_clock::now();
    m_world          = buildWorld(spec);
    m_worldFor       = logical;
    m_rendererDirty  = true;
    m_placements.clear();
    m_hold = {};

    m_screen.logicalSize = logical;
    m_screen.setAnchor(m_world.desktop);

    logf("world {} built for {}x{}: {} vertices, {} triangles in {} ms", m_world.name, logical.x, logical.y, m_world.vertices.size(), m_world.collision.triangleCount(),
         std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

// --------------------------------------------------------------------- maps

void CDesktop3D::dropPlacements() {
    const bool any = !m_placements.empty();
    m_placements.clear();
    m_hold = {};
    if (any)
        g_pCompositor->updateSuspendedStates();
}

void CDesktop3D::checkMapConfig() {
    const std::string map   = configuredMap();
    const float       scale = g_cfgMapScale ? g_cfgMapScale->value() : 0.f;
    const std::string key   = std::format("{}\n{}", map, scale);
    if (key == m_mapConfigured)
        return;
    const bool first = m_mapConfigured.empty();
    m_mapConfigured  = key;
    if (first && map.empty())
        return; // nothing configured, the courtyard is already there
    const std::string r = requestMap(map, scale);
    if (r.starts_with("error: "))
        notify(r.substr(7), true);
}

std::string CDesktop3D::requestMap(const std::string& path, float scale) {
    PHLMONITOR mon = m_mode != MODE_OFF ? m_monitor.lock() : nullptr;
    if (!mon)
        mon = Desktop::focusState()->monitor();

    if (path.empty() || path == "none") {
        m_mapLoader.cancel();
        m_mapPath.clear();
        if (!m_world.model)
            return "ok";
        m_world    = {};
        m_worldFor = {-1, -1};
        m_mapScale = 0;
        dropPlacements();
        if (m_mode != MODE_OFF && mon) {
            buildWorldFor(mon->m_size);
            resetPlayer();
        }
        notify("back to the courtyard");
        return "ok";
    }

    std::error_code ec;
    const auto      abs = std::filesystem::absolute(path, ec);
    if (ec || !std::filesystem::is_regular_file(abs, ec))
        return "error: no such map file: " + path;
    const std::string ext = abs.extension().string();
    if (ext != ".gltf" && ext != ".glb" && ext != ".GLTF" && ext != ".GLB")
        return "error: maps have to be glTF (.gltf or .glb), convert other formats first (see the README)";

    SMapRequest req;
    req.path     = abs.string();
    req.scale    = scale;
    req.compress = gl::textureCompression(); // Hyprland's context is current here
    if (mon && mon->m_size.y > 0)
        req.aspect = (float)(mon->m_size.x / mon->m_size.y);
    m_mapPath = req.path;
    m_mapLoader.load(req);
    notify("loading map " + abs.filename().string() + "...");
    return "loading";
}

void CDesktop3D::applyMap(SMapResult&& res) {
    for (const auto& l : res.log)
        log(l);
    if (res.req.path != m_mapPath)
        return; // something else was asked for since

    const std::string file = std::filesystem::path(res.req.path).filename().string();
    if (!res.world || !res.error.empty()) {
        notify(std::format("couldn't load {}: {}", file, res.error.empty() ? "unknown error" : res.error), true);
        m_mapPath = m_world.model ? m_world.model->path : "";
        return;
    }

    m_world         = std::move(*res.world);
    m_mapScale      = res.req.scale;
    m_rendererDirty = true;
    m_worldFor      = {-1, -1};
    dropPlacements();

    PHLMONITOR mon = m_mode != MODE_OFF ? m_monitor.lock() : nullptr;
    if (!mon)
        mon = Desktop::focusState()->monitor();
    if (mon)
        buildWorldFor(mon->m_size); // only sets up where the desktop hangs
    if (m_mode != MODE_OFF)
        resetPlayer();

    notify(std::format("map {} loaded ({} triangles)", m_world.name, m_world.model ? m_world.model->triangles : 0));
}

// ------------------------------------------------------------------ avatar

void CDesktop3D::checkAvatarConfig() {
    if (const int physics = !g_cfgAvatarPhysics || g_cfgAvatarPhysics->value(); physics != m_physicsConfigured) {
        m_physicsConfigured = physics;
        m_anim.setPhysics(physics);
    }
    if (const int lip = g_cfgLipSync && g_cfgLipSync->value(); lip != m_lipsyncConfigured) {
        const bool first    = m_lipsyncConfigured < 0;
        m_lipsyncConfigured = lip;
        if (!first || lip)
            setLipSync(lip);
    }

    // other emotes: the avatar that's there again with them
    std::string emotes;
    for (const auto& f : configuredEmotes())
        emotes += f + "\n";
    const bool emotesChanged = m_emotesConfigured != "\n" && emotes != m_emotesConfigured;
    m_emotesConfigured       = emotes;

    const std::string path   = configuredPath(g_cfgAvatar);
    const float       height = g_cfgAvatarHeight ? g_cfgAvatarHeight->value() : 0.f;
    const std::string key    = std::format("{}\n{}", path, height);
    if (key == m_avatarConfigured) {
        if (emotesChanged && !m_avatarPath.empty())
            requestAvatar(m_avatarPath, m_avatarHeight);
        return;
    }
    const bool first   = m_avatarConfigured.empty();
    m_avatarConfigured = key;
    if (first && path.empty())
        return;
    const std::string r = requestAvatar(path, height);
    if (r.starts_with("error: "))
        notify(r.substr(7), true);
}

std::string CDesktop3D::requestAvatar(const std::string& path, float height) {
    // what's being made is for the one there now; the next is made with them
    m_emoteLoader.cancel();
    m_emoteLoading.clear();
    m_emotePlay.clear();
    if (path.empty() || path == "none") {
        m_avatarLoader.cancel();
        m_avatarPath.clear();
        if (!m_avatar)
            return "ok";
        m_avatar.reset();
        m_ctl.avatar.reset();
        m_anim.reset(nullptr);
        m_thirdPerson = false;
        notify("avatar removed");
        return "ok";
    }

    std::error_code ec;
    const auto      abs = std::filesystem::absolute(path, ec);
    if (ec || !std::filesystem::is_regular_file(abs, ec))
        return "error: no such avatar file: " + path;
    std::string ext = abs.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext != ".gltf" && ext != ".glb" && ext != ".vrm")
        return "error: avatars have to be glTF, GLB or VRM; convert a VRChat .unitypackage first (see the README)";

    SAvatarRequest req;
    req.path     = abs.string();
    req.height   = height;
    req.emotes   = emoteFiles();
    m_avatarPath = req.path;
    m_avatarLoader.load(req);
    notify("loading avatar " + abs.filename().string() + "...");
    return "loading";
}

void CDesktop3D::applyAvatar(SAvatarResult&& res) {
    for (const auto& l : res.log)
        log(l);
    if (res.req.path != m_avatarPath)
        return; // something else was asked for since

    const std::string file = std::filesystem::path(res.req.path).filename().string();
    if (!res.model || !res.error.empty()) {
        notify(std::format("couldn't load avatar {}: {}", file, res.error.empty() ? "unknown error" : res.error), true);
        m_avatarPath = m_avatar ? m_avatar->path : "";
        return;
    }

    const bool first = !m_avatar;
    m_avatar         = std::move(res.model);
    m_avatarHeight   = res.req.height;
    m_anim.reset(m_avatar);
    m_ctl.avatar = m_avatar;
    m_ctl.loaded(); // what was set by hand, again
    m_bodyYaw          = m_yaw;
    m_lookYaw          = m_lookPitch = 0;
    m_avatarLight.full = false;
    if (first) {
        // that's what it's for
        m_thirdPerson = true;
        m_camBoom     = m_camDist;
    }
    notify(std::format("avatar {} loaded ({} triangles, {:.2f} m{}), V switches the view", m_avatar->name, m_avatar->triangles, m_avatar->height,
                       m_avatar->humanoid ? "" : ", not a humanoid"));
    // emote files that came since it was asked for
    std::vector<std::string> later;
    for (const auto& f : emoteFiles())
        if (std::ranges::find(res.req.emotes, f) == res.req.emotes.end())
            later.push_back(f);
    if (!later.empty())
        loadEmoteFiles(std::move(later));
}

std::vector<std::string> CDesktop3D::emoteFiles() const {
    std::vector<std::string> out;
    for (const auto& f : configuredEmotes()) {
        std::error_code ec;
        const auto      abs = std::filesystem::absolute(f, ec);
        out.push_back(ec ? f : abs.string());
    }
    for (const auto& f : m_emoteFiles)
        if (std::ranges::find(out, f) == out.end())
            out.push_back(f);
    return out;
}

// made on the side for the avatar there; with what was being made (a new load drops that)
void CDesktop3D::loadEmoteFiles(std::vector<std::string> files) {
    if (!m_avatar)
        return;
    for (const auto& f : m_emoteLoading)
        if (std::ranges::find(files, f) == files.end())
            files.push_back(f);
    SEmoteRequest req;
    for (const auto& f : files)
        req.path += (req.path.empty() ? "" : ", ") + std::filesystem::path(f).filename().string();
    req.files      = files;
    req.model      = m_avatar;
    m_emoteLoading = std::move(files);
    m_emoteLoader.load(req);
}

void CDesktop3D::applyEmotes(SEmoteResult&& res) {
    for (const auto& l : res.log)
        log(l);
    if (!m_avatar || res.req.model != m_avatar)
        return; // for one that's gone
    m_emoteLoading.clear();
    const bool play = !m_emotePlay.empty() && std::ranges::find(res.req.files, m_emotePlay) != res.req.files.end();
    if (res.emotes.empty()) {
        // not again with the next avatar
        std::erase_if(m_emoteFiles, [&](const std::string& f) { return std::ranges::find(res.req.files, f) != res.req.files.end(); });
        if (play)
            m_emotePlay.clear();
        notify("couldn't make emotes: " + (res.error.empty() ? std::string("unknown error") : res.error), true);
        return;
    }
    int         first = -1;
    std::string names;
    for (const auto& e : res.emotes) {
        const int i = m_anim.addEmote(e);
        if (i < 0)
            continue;
        if (first < 0 && play && e->from == std::filesystem::path(m_emotePlay).filename().string())
            first = i;
        names += (names.empty() ? "" : ", ") + e->name;
    }
    if (play) {
        // a folder's: the first of it
        if (first < 0)
            first = m_anim.findEmote(res.emotes.front()->name);
        m_anim.playEmote(first, m_emotePlayLoop);
        m_emotePlay.clear();
    }
    notify(std::format("emotes: {}{}", names, res.error.empty() ? "" : " (" + res.error + ")"));
}

std::string CDesktop3D::avatarStatus() const {
    if (!m_avatar)
        return std::format(R"({{"path": "{}", "loading": {}, "view": "first"}})", jsonEscape(m_avatarPath), m_avatarLoader.busy());
    const auto& a = *m_avatar;
    static constexpr const char* EYES[] = {"still", "bones", "expressions"};
    const int                    held   = m_anim.expression();
    const int emote = m_anim.emote();
    return std::format(R"({{"path": "{}", "loading": {}, "name": "{}", "triangles": {}, "joints": {}, "height": {:.3f}, "humanoid": {}, "rig": "{}", "clips": {}, "playing": "{}", "view": "{}", "distance": {:.2f}, "bodyYaw": {:.1f}, "light": [{:.2f}, {:.2f}], "expressions": {}, "expressionsFrom": "{}", "expression": "{}", "gestures": ["{}", "{}"], "eyes": "{}", "parts": {}, "toggles": {}, "sliders": {}, "variants": {}, "settings": "{}", "physics": {}, "springs": {}, "springBones": {}, "springsFrom": "{}", "constraints": {}, "emote": "{}", "emotes": {}}})",
                       jsonEscape(m_avatarPath), m_avatarLoader.busy(), jsonEscape(a.name), a.triangles, a.joints.size(), a.height, a.humanoid, jsonEscape(a.humanFrom),
                       a.clips.size(), jsonEscape(m_anim.playing()), m_thirdPerson ? "third" : "first", m_camBoom, m_bodyYaw * 180.f / F_PI, m_avatarLight.skyAvg,
                       m_avatarLight.bounceAvg, a.expressions.size(), jsonEscape(a.expressionsFrom), held >= 0 ? jsonEscape(a.expressions[held].name) : "",
                       gestureName(m_anim.gesture(0)), gestureName(m_anim.gesture(1)), EYES[a.lookAt.type], a.parts.size(), a.toggles.size(), a.sliders.size(), a.variants.size(), jsonEscape(a.settings),
                       m_anim.physics(), a.springs.size(), a.springJoints.size(), jsonEscape(a.springsFrom), a.constraints.size(),
                       emote >= 0 ? jsonEscape(m_anim.emotes()[emote]->name) : "", m_anim.emotes().size());
}

// hyprctl hypr3d avatar emote FILE|FOLDER: made for the avatar there, then its first emote plays
std::string CDesktop3D::loadEmoteFile(const std::string& file, int loop) {
    if (std::ranges::find(m_emoteFiles, file) == m_emoteFiles.end())
        m_emoteFiles.push_back(file);
    m_emotePlay     = file;
    m_emotePlayLoop = loop;
    loadEmoteFiles({file});
    return "loading";
}

std::string CDesktop3D::setView(bool third) {
    if (third && !m_avatar)
        return m_avatarLoader.busy() ? "error: the avatar is still loading" : "error: no avatar loaded (set plugin:hypr3d:avatar or use hyprctl hypr3d avatar <file>)";
    if (third && !m_thirdPerson)
        m_camBoom = 0.3f; // pull out from the head
    m_thirdPerson = third;
    return third ? "third" : "first";
}

M4 CDesktop3D::avatarTransform() const {
    return M4::trs(m_feet + V3{0, m_anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -m_bodyYaw), {1, 1, 1});
}

// how much sky and how much sunlight bounced off the surroundings the avatar
// gets, like the map's baked values: a few rays a frame from its chest
void CDesktop3D::measureAvatarLight() {
    static const auto DIRS = [] {
        std::array<V3, 32> d;
        for (size_t i = 0; i < d.size(); ++i) {
            const float y = 1.f - (i + 0.5f) * 2.f / d.size();
            const float r = std::sqrt(std::max(0.f, 1.f - y * y));
            const float a = i * 2.39996323f; // golden angle
            d[i]          = {r * std::cos(a), y, r * std::sin(a)};
        }
        return d;
    }();

    auto&       L     = m_avatarLight;
    const V3    from  = m_feet + V3{0, std::max(0.3f, m_avatar->height * 0.55f), 0};
    const V3    sun   = m_world.sunDir;
    const auto  probe = [&](size_t i) {
        const V3 d = DIRS[i];
        SRayHit  h;
        L.sky[i] = L.bounce[i] = 0;
        if (!m_world.collision.raycast(from, d, 60.f, h)) {
            L.sky[i] = 1;
            return;
        }
        const float lit = dot(h.normal, sun);
        if (SRayHit s; lit > 0.f && !m_world.collision.raycast(from + d * h.t + h.normal * 0.02f, sun, 300.f, s))
            L.bounce[i] = lit;
    };
    for (int n = L.full ? 4 : (int)DIRS.size(); n > 0; --n) {
        probe(L.next);
        L.next = (L.next + 1) % DIRS.size();
    }
    L.full = true;

    // sky: seen by the upper half, weighted like the map's cosine hemisphere; bounce: all around
    float sky = 0, skyW = 0, bounce = 0;
    for (size_t i = 0; i < DIRS.size(); ++i) {
        if (DIRS[i].y > 0) {
            sky += L.sky[i] * DIRS[i].y;
            skyW += DIRS[i].y;
        }
        bounce += L.bounce[i];
    }
    L.skyAvg    = skyW > 0 ? sky / skyW : 1.f;
    L.bounceAvg = std::min(bounce / DIRS.size(), 1.f);
}

// the body turns to where it walks, the head to where the camera looks (or at the camera)
void CDesktop3D::animateAvatar(float dt) {
    if (!m_avatar)
        return;

    const V3    hv{m_vel.x, 0, m_vel.z};
    const float speed = m_mode == MODE_ACTIVE ? length(hv) : 0.f;
    float       want  = m_bodyYaw;
    if (speed > 0.3f) {
        want          = m_thirdPerson ? std::atan2(hv.x, -hv.z) : m_yaw;
        m_bodyTurning = false;
    } else if (!m_thirdPerson) {
        // standing: the body catches up once the head would turn too far
        const float rel = wrapAngle(m_yaw - m_bodyYaw);
        if (std::abs(rel) > 0.9f)
            m_bodyTurning = true;
        if (m_bodyTurning) {
            want = m_yaw;
            if (std::abs(rel) < 0.05f)
                m_bodyTurning = false;
        }
    }
    m_bodyYaw = wrapAngle(m_bodyYaw + wrapAngle(want - m_bodyYaw) * (1.f - std::exp(-dt * 10.f)));

    float lookYaw = wrapAngle(m_camera.yaw - m_bodyYaw), lookPitch = m_camera.pitch;
    if (m_thirdPerson && std::abs(lookYaw) > 1.75f) {
        // the camera looks at its face: look back at it
        const V3 d = m_camera.eye - (m_feet + V3{0, m_avatar->height * 0.92f, 0});
        lookYaw    = wrapAngle(std::atan2(d.x, -d.z) - m_bodyYaw);
        lookPitch  = std::atan2(d.y, std::hypot(d.x, d.z));
    }
    const float k = 1.f - std::exp(-dt * 6.f);
    m_lookYaw += (std::clamp(lookYaw, -1.4f, 1.4f) - m_lookYaw) * k;
    m_lookPitch += (std::clamp(lookPitch, -1.1f, 1.1f) - m_lookPitch) * k;

    SAvatarMotion mo;
    mo.dt        = dt;
    mo.speed     = speed;
    mo.vy        = m_vel.y;
    mo.onGround  = m_onGround || m_mode != MODE_ACTIVE;
    mo.flying    = m_fly;
    mo.crouched  = m_crouched;
    mo.lookYaw   = m_lookYaw;
    mo.lookPitch = m_lookPitch;
    mo.world     = M4::trs(m_feet, Quat::axisAngle({0, 1, 0}, -m_bodyYaw), {1, 1, 1});
    m_anim.update(mo);

    measureAvatarLight();
}

// hangs the desktop on whatever the crosshair points at
std::string CDesktop3D::placeDesktop(float height) {
    if (m_mode != MODE_ACTIVE)
        return "error: not in 3D";
    const V3 eye = m_camera.eye;
    const V3 dir = forwardFrom(m_camera.yaw, m_camera.pitch);
    SRayHit  hit;
    if (!m_world.collision.raycast(eye, dir, 100.f, hit))
        return "error: not looking at anything";

    V3 n = hit.normal;
    n.y  = 0;
    if (length(n) < 0.3f)
        return "error: that's a floor or a ceiling, look at a wall";
    n = normalize(n);

    SDesktopAnchor d;
    d.height = height > 0.1f ? height : m_world.desktop.height;
    d.center = eye + dir * hit.t + n * 0.01f; // centered where the crosshair is
    d.normal = n;

    m_world.desktop = d;
    dropPlacements();
    m_screen.setAnchor(d);
    if (!m_mapPath.empty() && m_world.model)
        saveMapState(m_mapPath, m_world, m_mapScale);
    return std::format("desktop at {:.2f} {:.2f} {:.2f}, {:.2f} m tall", d.center.x, d.center.y, d.center.z, d.height);
}

bool CDesktop3D::enter(PHLMONITOR mon) {
    if (m_mode == MODE_ENTERING || m_mode == MODE_ACTIVE)
        return true;
    if (m_mode == MODE_EXITING)
        return false;

    if (g_pSessionLockManager->isSessionLocked())
        return false;
    if (m_rendererFailed) {
        notify("the 3D renderer failed to start earlier, check the Hyprland log", true);
        return false;
    }

    if (!mon)
        mon = Desktop::focusState()->monitor();
    if (!mon)
        mon = g_pCompositor->getMonitorFromCursor();
    if (!mon || mon->m_size.x < 1 || mon->m_size.y < 1)
        return false;

    buildWorldFor(mon->m_size);

    if (m_restoreLater)
        m_restoreLater.reset(); // still in the "leaving 3D" state, nothing to save again
    else
        m_prevDSBlocked = g_pHyprRenderer->m_directScanoutBlocked;

    m_monitor = mon;
    m_mode    = MODE_ENTERING;
    m_t       = 0;
    resetPlayer();
    m_typing     = false;
    m_look       = {};
    m_lastAbs    = {-1, -1};
    m_lastUpdate = std::chrono::steady_clock::now();
    m_keys.fill(false);

    g_pHyprRenderer->m_directScanoutBlocked = true;
    g_pHyprRenderer->setCursorHidden(true);
    g_pHyprRenderer->damageMonitor(mon);
    g_pCompositor->scheduleFrameForMonitor(mon);

    log("entering 3D on " + mon->m_name);
    return true;
}

void CDesktop3D::exit(bool immediate) {
    if (m_mode == MODE_OFF)
        return;

    if (immediate) {
        exitNow();
        return;
    }

    if (m_mode == MODE_EXITING)
        return;

    m_exitFrom = m_camera;
    if (m_mode == MODE_ACTIVE)
        m_t = 1.f;
    m_mode   = MODE_EXITING;
    m_typing = false;
    m_menu.hide();
    m_drag   = {};
    if (m_hold.key)
        place(); // whatever is being carried stays where it is

    if (const auto mon = m_monitor.lock())
        g_pCompositor->scheduleFrameForMonitor(mon);
}

void CDesktop3D::toggle() {
    if (m_mode == MODE_OFF)
        enter();
    else
        exit();
}

// stops drawing right away; the rest of the cleanup runs outside of rendering
void CDesktop3D::exitNow() {
    if (m_mode == MODE_OFF)
        return;
    m_mode = MODE_OFF;
    m_t    = 0;
    m_menu.hide();
    lipSync(); // out of 3D: the microphone closes (update() no longer runs)
    if (!m_restoreLater)
        m_restoreLater = g_pEventLoopManager->doLaterLock([this] {
            m_restoreLater.reset(); // safe: the queue already moved this callback out
            restore();
        });
    if (const auto mon = m_monitor.lock())
        g_pHyprRenderer->damageMonitor(mon);
}

void CDesktop3D::restore() {
    const uint32_t t = nowMs();
    for (uint32_t b : m_sentButtons)
        g_pSeatManager->sendPointerButton(t, b, WL_POINTER_BUTTON_STATE_RELEASED);
    if (!m_sentButtons.empty())
        g_pSeatManager->sendPointerFrame();
    m_sentButtons.clear();
    m_drag   = {};
    m_typing = false;
    m_keys.fill(false);
    if (m_hold.key)
        place();
    // placed windows were kept awake on hidden workspaces, let Hyprland suspend them again
    if (!m_placements.empty())
        g_pCompositor->updateSuspendedStates();
    m_aimed = -1;
    m_aimSurface.reset();
    m_lastSentLocal = {-1, -1};
    m_panels.clear();

    g_pHyprRenderer->m_directScanoutBlocked = m_prevDSBlocked;
    g_pHyprRenderer->setCursorHidden(false);
    g_pHyprRenderer->ensureCursorRenderingMode();
    g_pInputManager->simulateMouseMovement();

    if (const auto mon = m_monitor.lock())
        g_pHyprRenderer->damageMonitor(mon);

    log("left 3D");
}

void CDesktop3D::setTyping(bool on) {
    if (m_mode != MODE_ACTIVE)
        return;
    m_typing = on;
    m_keys.fill(false);
    if (on)
        m_menu.hide();

    if (on && m_aimed >= 0 && m_aimed < (int)m_panels.size()) {
        const auto w = m_panels[m_aimed].window.lock();
        if (w && w != Desktop::focusState()->window())
            Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK);
    }
}

void CDesktop3D::resetPlayer() {
    m_feet      = m_world.spawn;
    m_vel       = {};
    m_yaw       = m_world.spawnYaw;
    m_pitch     = 0;
    m_eyeHeight = EYE;
    m_onGround  = true;
    m_crouched  = false;
    m_fly       = false;
    m_lightFull = false;
    m_bodyYaw     = m_yaw;
    m_bodyTurning = false;
    m_camBoom     = m_camDist;
    m_avatarLight.full = false;
}

// Roughly how bright the surroundings look, from rays in every direction:
// what the sun lights directly, plus some ambient, or the sky. The exposure
// follows, the way eyes adapt: within the map's own range when it has one (a
// CS2 map's post processing volume: hardly at all), else dark places get
// somewhat brighter. Only the world is brightened, never the windows. The
// test harness's --autoexp does the same through the same functions (map.cpp).
void CDesktop3D::adaptExposure(float dt) {
    if (!m_world.model) {
        m_exposure = 1;
        return;
    }

    const bool snap = !m_lightFull;
    for (int n = snap ? EXPOSURE_SAMPLES : 8; n > 0; --n) {
        m_lightProbe[m_lightNext] = exposureSample(m_world, m_camera.eye, exposureDirection(m_lightNext));
        m_lightNext               = (m_lightNext + 1) % EXPOSURE_SAMPLES;
    }
    m_lightFull = true;

    m_lightAvg         = std::max(std::accumulate(m_lightProbe.begin(), m_lightProbe.end(), 0.f) / m_lightProbe.size(), 1e-3f);
    const float target = exposureFor(m_world, m_lightProbe.data(), forwardFrom(m_camera.yaw, m_camera.pitch));
    m_exposure         = snap ? target : exposureStep(m_world, m_exposure, target, dt);
}

// ----------------------------------------------------------------- cameras

// the camera at which the wall with the desktop exactly fills the screen
SCamera CDesktop3D::flatCamera() const {
    const float dist = (m_screen.height * 0.5f) / std::tan(FOV_Y * 0.5f);
    return {m_screen.center + m_screen.normal * dist, std::atan2(-m_screen.normal.x, m_screen.normal.z), 0.f};
}

SCamera CDesktop3D::playerCamera() const {
    return {m_feet + V3{0, m_eyeHeight, 0}, m_yaw, m_pitch};
}

// in third person the camera looks the same way, from behind the avatar's
// shoulder; walls pull it in so it never sees through them
SCamera CDesktop3D::viewCamera(float dt) {
    SCamera c = playerCamera();
    if (!m_thirdPerson || !m_avatar)
        return c;

    const V3    fwd = forwardFrom(m_yaw, m_pitch);
    const V3    right{std::cos(m_yaw), 0, std::sin(m_yaw)};
    const V3    up    = cross(right, fwd);
    const float crouch = m_eyeHeight / EYE;
    const V3    head   = m_feet + V3{0, std::max(0.5f, m_avatar->height * 0.95f * crouch + 0.15f), 0};

    // the shoulder offset, unless that's inside a wall
    float   side = m_camSide;
    SRayHit hit;
    if (side > 0 && m_world.collision.raycast(head, right, side + 0.25f, hit))
        side = std::max(0.f, hit.t - 0.25f);
    const V3 pivot = head + right * side;

    // a few rays around the boom, a thick one so the near plane stays out of walls too
    constexpr float R    = 0.18f;
    float           want = m_camDist;
    for (const V3& o : {V3{}, right * R, right * -R, up * R, up * -R}) {
        if (m_world.collision.raycast(pivot + o, -fwd, want + 0.2f, hit))
            want = std::max(0.f, hit.t - 0.2f);
    }
    // in at once, out gently
    m_camBoom = want < m_camBoom ? want : m_camBoom + (want - m_camBoom) * (1.f - std::exp(-dt * 3.f));
    c.eye     = pivot - fwd * m_camBoom;
    return c;
}

// ------------------------------------------------------------------ update

void CDesktop3D::update() {
    const auto now = std::chrono::steady_clock::now();
    const float rawDt = std::chrono::duration<float>(now - m_lastUpdate).count();
    const float dt    = std::clamp(rawDt, 0.f, 0.05f);
    m_lastUpdate      = now;
    m_time += dt;
    ++m_frames;
    m_minDt = std::min(m_minDt, rawDt);
    if (const float since = std::chrono::duration<float>(now - m_lastFps).count(); since >= 1.f) {
        m_fps             = (m_frames - m_framesAtLastFps) / since;
        m_framesAtLastFps = m_frames;
        m_lastFps         = now;
    }

    switch (m_mode) {
        case MODE_ENTERING:
            m_look = {};
            m_t += dt / ENTER_TIME;
            if (m_t >= 1.f) {
                m_t    = 1.f;
                m_mode = MODE_ACTIVE;
            }
            break;
        case MODE_EXITING:
            m_look = {};
            m_t -= dt / EXIT_TIME;
            if (m_t <= 0.f) {
                exitNow();
                return;
            }
            break;
        case MODE_ACTIVE: simulate(dt); break;
        default: return;
    }

    const auto mon = m_monitor.lock();
    if (!mon) {
        exitNow();
        return;
    }
    if (mon->m_size != m_worldFor) {
        // the monitor changed resolution or scale under us
        exitNow();
        return;
    }

    const float e = smoothstep01(m_t);
    m_depthMul    = e;

    const SCamera view = viewCamera(dt);
    if (m_mode == MODE_ACTIVE)
        m_camera = view;
    else {
        const SCamera a = flatCamera();
        const SCamera b = m_mode == MODE_ENTERING ? view : m_exitFrom;
        m_camera.eye    = lerp(a.eye, b.eye, e);
        m_camera.yaw    = a.yaw + wrapAngle(b.yaw - a.yaw) * e; // the short way round
        m_camera.pitch  = lerpf(a.pitch, b.pitch, e);
    }

    adaptExposure(dt);
    lipSync();
    animateAvatar(dt);
    m_menu.update(dt, (int)std::round(mon->m_transformedSize.x), (int)std::round(mon->m_transformedSize.y), (float)mon->m_scale);

    const V3 fwd = forwardFrom(m_camera.yaw, m_camera.pitch);
    m_view       = M4::lookAt(m_camera.eye, m_camera.eye + fwd, {0, 1, 0});
    const float far = m_world.model ? std::max(200.f, length(m_world.bounds.size()) * 1.5f) : 200.f;
    m_proj          = M4::perspective(FOV_Y, (float)(mon->m_size.x / mon->m_size.y), 0.05f, far);

    std::unordered_set<uintptr_t> placed;
    for (const auto& [key, pl] : m_placements)
        placed.insert(key);
    m_panels = collectPanels(mon, layerSpacing(), placed);
    updatePlacements(dt);
    layoutPanels(e);

    if (m_mode == MODE_ACTIVE) {
        aim();
        updatePointer();
    } else
        m_aimed = -1;
}

bool CDesktop3D::overlaps(const V3& feet, float height) const {
    const SAABB me{{feet.x - RADIUS, feet.y, feet.z - RADIUS}, {feet.x + RADIUS, feet.y + height, feet.z + RADIUS}};

    // keep out of the windows hanging in front of the desktop wall
    const float halfW = (float)(m_screen.logicalSize.x * m_screen.scale()) * 0.5f;
    const float halfH = m_screen.height * 0.5f;
    const V3    d     = feet + V3{0, height * 0.5f, 0} - m_screen.center;
    const float x = dot(d, m_screen.right), y = dot(d, m_screen.up), z = dot(d, m_screen.normal);
    const float guardDepth = 7.f * layerSpacing() + 0.1f;
    if (std::abs(x) < halfW + 0.1f + RADIUS && std::abs(y) < halfH + 0.1f + height * 0.5f && z > -1.f - RADIUS && z < guardDepth + RADIUS)
        return true;

    return m_world.collision.overlaps(me);
}

// --------------------------------------------------------- placing windows

namespace {
    SPanelPose poseFrom(const V3& center, const Quat& rot, float scale, const CBox& box) {
        SPanelPose p;
        p.right  = rot.rotate({1, 0, 0});
        p.down   = -rot.rotate({0, 1, 0});
        p.normal = rot.rotate({0, 0, 1});
        p.scale  = scale;
        p.origin = center - p.right * (float)(box.w * 0.5 * scale) - p.down * (float)(box.h * 0.5 * scale);
        return p;
    }

    float quatAngle(const Quat& a, const Quat& b) {
        const float d = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        return 2.f * std::acos(std::min(d, 1.f));
    }

    // distance from `eye` to the nearest point of a panel
    float panelDistance(const V3& eye, const SPanelPose& pose, const CBox& box) {
        const V3    d = eye - pose.origin;
        const float x = std::clamp(dot(d, pose.right) / pose.scale, 0.f, (float)box.w);
        const float y = std::clamp(dot(d, pose.down) / pose.scale, 0.f, (float)box.h);
        return length(eye - pose.at({x, y}));
    }
}

// where a window sits on the desktop wall once fully in 3D
SPlacement CDesktop3D::layoutPlacement(const SPanel& p) const {
    SPlacement pl;
    pl.center      = m_screen.pose(p.box, p.depth).at(p.box.size() * 0.5);
    pl.rot         = Quat::fromBasis(m_screen.right, m_screen.up, m_screen.normal);
    pl.scale       = m_screen.scale();
    pl.targetCenter = pl.center;
    pl.targetRot    = pl.rot;
    pl.targetScale  = pl.scale;
    return pl;
}

void CDesktop3D::updatePlacements(float dt) {
    std::erase_if(m_placements, [](const auto& kv) { return kv.second.window.expired(); });
    if (m_hold.key && !m_placements.contains(m_hold.key))
        m_hold = {};
    if (m_placements.empty())
        return;

    const auto mon = m_monitor.lock();
    const auto now = Time::steadyNow();
    const V3   eye = m_camera.eye;
    const V3   dir = forwardFrom(m_camera.yaw, m_camera.pitch);

    std::unordered_set<uintptr_t> seen;
    for (const auto& p : m_panels) {
        if (p.kind != PANEL_WINDOW)
            continue;
        auto it = m_placements.find(p.key);
        if (it == m_placements.end())
            continue;
        auto& pl = it->second;
        seen.insert(p.key);

        if (pl.returning) {
            // the layout may have moved it meanwhile
            const SPlacement home = layoutPlacement(p);
            pl.targetCenter       = home.center;
            pl.targetRot          = home.rot;
            pl.targetScale        = home.scale;
        } else if (p.key == m_hold.key) {
            const float scale = m_screen.scale() * m_hold.scaleMul;
            const V3    camRight{std::cos(m_camera.yaw), 0, std::sin(m_camera.yaw)};
            const V3    camUp = cross(camRight, dir);

            V3      c, r, u, n;
            float   pull = 1.f;
            SRayHit hit;
            if (m_world.collision.raycast(eye, dir, m_hold.dist, hit)) {
                // flat against whatever it touches: upright on walls, on floors and
                // ceilings turned so it reads the right way from where you stand
                n = hit.normal;
                const V3 upHint = std::abs(n.y) > 0.85f ? camUp : V3{0, 1, 0};
                u               = normalize(upHint - n * dot(upHint, n));
                if (length(u) < 0.5f)
                    u = perpendicular(n);
                r = normalize(cross(u, n));
                c = eye + dir * hit.t + n * 0.02f;

                // slide it out of corners: a window on a wall shouldn't sink into the floor
                const float half[2] = {(float)p.box.w * 0.5f * scale, (float)p.box.h * 0.5f * scale};
                for (int pass = 0; pass < 2; ++pass) {
                    for (int k = 0; k < 4; ++k) {
                        const V3    axis = (k < 2 ? r : u) * (k % 2 ? -1.f : 1.f);
                        const float ext  = half[k / 2];
                        if (SRayHit h2; m_world.collision.raycast(c, axis, ext, h2))
                            c -= axis * (ext - h2.t + 0.005f);
                    }
                }
            } else {
                // floating in front of you, facing you
                n = -dir;
                u = camUp;
                r = camRight;
                c = eye + dir * m_hold.dist;

                // pull it towards you (same size on screen) instead of letting it sink into
                // things: anything between you and some point of the window is in the way
                const float hw = (float)p.box.w * 0.5f * scale, hh = (float)p.box.h * 0.5f * scale;
                constexpr int NX = 17, NY = 9;
                for (int iy = 0; iy < NY; ++iy) {
                    for (int ix = 0; ix < NX; ++ix) {
                        const V3    point = c + r * (hw * (2.f * ix / (NX - 1) - 1.f)) + u * (hh * (2.f * iy / (NY - 1) - 1.f)) + n * 0.01f;
                        const V3    d     = point - eye;
                        const float len   = length(d);
                        if (SRayHit h2; len > 1e-3f && m_world.collision.raycast(eye, d / len, len, h2))
                            pull = std::min(pull, (h2.t - 0.04f) / len);
                    }
                }
                pull = std::max(pull, 0.1f);
            }

            pl.targetCenter = eye + (c - eye) * pull;
            pl.targetRot    = Quat::fromBasis(r, u, n);
            pl.targetScale  = scale * pull;
        }

        // windows on workspaces that aren't shown are suspended and get no frame
        // callbacks from Hyprland, which would freeze them
        if (mon) {
            const auto w = pl.window.lock();
            if (w && !g_pHyprRenderer->shouldRenderWindow(w, mon)) {
                w->setSuspended(false);
                if (w->wlSurface() && w->wlSurface()->resource())
                    w->wlSurface()->resource()->breadthfirst([&now](SP<CWLSurfaceResource> s, const Vector2D&, void*) { s->frame(now); }, nullptr);
            }
        }
    }

    // back to Hyprland's rules: suspended when its workspace isn't shown
    auto release = [](const SPlacement& pl) {
        if (const auto w = pl.window.lock(); w && w->m_isMapped)
            w->setSuspended(w->isHidden() || !w->m_workspace || !w->m_workspace->isVisible());
    };

    const float k = 1.f - std::exp(-dt * 18.f);
    for (auto it = m_placements.begin(); it != m_placements.end();) {
        auto& pl = it->second;
        if (!seen.contains(it->first) && pl.returning) {
            release(pl);
            it = m_placements.erase(it); // not on screen to fly back to, it's home anyway
            continue;
        }
        pl.center  = lerp(pl.center, pl.targetCenter, k);
        pl.rot     = slerp(pl.rot, pl.targetRot, k);
        pl.scale   = lerpf(pl.scale, pl.targetScale, k);
        pl.settled = length(pl.center - pl.targetCenter) < 0.002f && quatAngle(pl.rot, pl.targetRot) < 0.005f && std::abs(pl.scale - pl.targetScale) < pl.targetScale * 0.002f;
        if (pl.returning && pl.settled) {
            release(pl);
            it = m_placements.erase(it);
            continue;
        }
        ++it;
    }
}

// poses for everything, and the drawing order
void CDesktop3D::layoutPanels(float e) {
    const bool wallpaper = g_cfgWallpaper && g_cfgWallpaper->value();
    // close to the flat desktop everything is stacked like in 2D, placed windows included
    const bool inWorld = e >= 0.2f;
    const CBox monitor{{0, 0}, m_screen.logicalSize};
    const V3   eye     = m_camera.eye;
    const Quat wallRot = Quat::fromBasis(m_screen.right, m_screen.up, m_screen.normal);

    std::unordered_map<uintptr_t, size_t> windows;
    for (size_t i = 0; i < m_panels.size(); ++i)
        if (m_panels[i].kind == PANEL_WINDOW)
            windows[m_panels[i].key] = i;

    for (auto& p : m_panels) {
        // in 3D the wallpaper gives way to the world's own wall
        if (p.layer == 0 && !wallpaper)
            p.alpha *= 1.f - e;

        uintptr_t owner = 0;
        if (p.kind == PANEL_WINDOW)
            owner = p.key;
        else if (p.kind == PANEL_POPUP)
            if (const auto w = p.window.lock())
                owner = reinterpret_cast<uintptr_t>(w.get());
        const auto pit = owner ? m_placements.find(owner) : m_placements.end();

        if (pit == m_placements.end()) {
            p.pose = m_screen.pose(p.box, p.depth * e);
            // the desktop ends at the monitor edge, like the real screen
            const CBox vis = p.box.intersection(monitor);
            p.clip         = vis.w >= 1 && vis.h >= 1 ? CBox{vis.pos() - p.box.pos(), vis.size()} : CBox{};
            continue;
        }

        const auto&   pl = pit->second;
        const auto    wi = windows.find(owner);
        const SPanel& o  = wi != windows.end() ? m_panels[wi->second] : p;

        // fly between the wall and the placement while going in and out of 3D
        const V3         wallCenter = m_screen.pose(o.box, o.depth * e).at(o.box.size() * 0.5);
        const SPanelPose ownerPose  = poseFrom(lerp(wallCenter, pl.center, e), slerp(wallRot, pl.rot, e), lerpf(m_screen.scale(), pl.scale, e), o.box);

        p.pose = ownerPose;
        if (&o != &p) {
            p.pose.origin = ownerPose.at(p.box.pos() - o.box.pos()) + ownerPose.normal * 0.01f;
        }
        p.clip       = CBox{{0, 0}, p.box.size()};
        p.placed     = inWorld;
        p.depthWrite = inWorld;
        p.held       = owner == m_hold.key && p.kind == PANEL_WINDOW;
        p.group      = owner;
        p.sortDist   = panelDistance(eye, ownerPose, o.box);
    }

    if (inWorld) {
        // desktop wall first as stacked in 2D, then placed windows far to near, popups over their window
        std::ranges::stable_sort(m_panels, [](const SPanel& a, const SPanel& b) {
            if (a.placed != b.placed)
                return !a.placed;
            if (!a.placed)
                return false;
            if (a.sortDist != b.sortDist)
                return a.sortDist > b.sortDist;
            if (a.group != b.group)
                return a.group < b.group;
            return a.kind != PANEL_POPUP && b.kind == PANEL_POPUP;
        });
    }
}

void CDesktop3D::grab() {
    if (m_aimed < 0 || m_aimed >= (int)m_panels.size())
        return;
    const auto& aimed = m_panels[m_aimed];
    const auto  w     = aimed.window.lock();
    if (!w || aimed.kind == PANEL_LAYER)
        return;
    const uintptr_t key = reinterpret_cast<uintptr_t>(w.get());

    const SPanel* owner = nullptr;
    for (const auto& p : m_panels)
        if (p.kind == PANEL_WINDOW && p.key == key)
            owner = &p;
    if (!owner)
        return;

    float    t = 2.f;
    Vector2D local;
    aimed.pose.intersect(m_camera.eye, forwardFrom(m_camera.yaw, m_camera.pitch), t, local, false);

    const auto it    = m_placements.find(key);
    m_hold.hadBefore = it != m_placements.end() && !it->second.returning;
    if (m_hold.hadBefore)
        m_hold.before = it->second;

    // pick it up from wherever it is drawn right now
    SPlacement& pl = m_placements[key];
    pl.window      = w;
    pl.center      = owner->pose.at(owner->box.size() * 0.5);
    pl.rot         = Quat::fromBasis(owner->pose.right, -owner->pose.down, owner->pose.normal);
    pl.scale       = owner->pose.scale;
    pl.targetCenter = pl.center;
    pl.targetRot    = pl.rot;
    pl.targetScale  = pl.scale;
    pl.returning    = false;

    m_hold.key      = key;
    m_hold.dist     = std::clamp(t, 0.8f, 6.f);
    m_hold.scaleMul = pl.scale / m_screen.scale();

    // let go of anything the client thinks is pressed
    const uint32_t now = nowMs();
    for (uint32_t b : m_sentButtons)
        g_pSeatManager->sendPointerButton(now, b, WL_POINTER_BUTTON_STATE_RELEASED);
    if (!m_sentButtons.empty())
        g_pSeatManager->sendPointerFrame();
    m_sentButtons.clear();
    m_drag = {};
}

void CDesktop3D::place() {
    // it keeps sliding into the spot it was last aimed at
    m_hold = {};
}

void CDesktop3D::cancelHold() {
    const uintptr_t  key    = m_hold.key;
    const bool       had    = m_hold.hadBefore;
    const SPlacement before = m_hold.before;
    m_hold                  = {};
    if (!had) {
        returnToWall(key);
        return;
    }
    if (auto it = m_placements.find(key); it != m_placements.end()) {
        it->second.targetCenter = before.targetCenter;
        it->second.targetRot    = before.targetRot;
        it->second.targetScale  = before.targetScale;
    }
}

void CDesktop3D::returnToWall(uintptr_t key) {
    if (m_hold.key == key)
        m_hold = {};
    if (auto it = m_placements.find(key); it != m_placements.end())
        it->second.returning = true;
}

void CDesktop3D::moveAxis(int axis, float d, bool& blocked) {
    blocked = false;
    if (d == 0.f)
        return;
    const float h = m_crouched ? HEIGHT_CROUCH : HEIGHT;

    // small steps so nothing is tunneled through
    const int   steps = std::max(1, (int)std::ceil(std::abs(d) / 0.1f));
    const float step  = d / steps;
    for (int i = 0; i < steps; ++i) {
        V3 p = m_feet;
        (&p.x)[axis] += step;
        if (!overlaps(p, h)) {
            m_feet = p;
            continue;
        }

        // walk up small ledges (stairs, the platform step)
        if (axis != 1 && m_onGround && !m_fly) {
            V3 up = p;
            up.y += STEP_HEIGHT;
            V3 upFrom = m_feet;
            upFrom.y += STEP_HEIGHT;
            if (!overlaps(upFrom, h) && !overlaps(up, h)) {
                m_feet = up;
                bool dummy = false;
                const bool wasGround = m_onGround;
                moveAxis(1, -STEP_HEIGHT, dummy); // settle on top of it
                m_onGround = wasGround;
                continue;
            }
        }

        // binary search the contact point for a snug stop
        float lo = 0.f, hi = 1.f;
        for (int k = 0; k < 8; ++k) {
            const float mid = (lo + hi) * 0.5f;
            V3          q   = m_feet;
            (&q.x)[axis] += step * mid;
            if (overlaps(q, h))
                hi = mid;
            else
                lo = mid;
        }
        (&m_feet.x)[axis] += step * lo;
        blocked = true;
        return;
    }
}

void CDesktop3D::simulate(float dt) {
    // look
    m_yaw += (float)m_look.x * m_sens;
    m_pitch -= (float)m_look.y * m_sens;
    m_look = {};
    if (!m_typing) {
        m_yaw += ((m_keys[K_RIGHT] ? 1.f : 0.f) - (m_keys[K_LEFT] ? 1.f : 0.f)) * 2.2f * dt;
        m_pitch += ((m_keys[K_UP] ? 1.f : 0.f) - (m_keys[K_DOWN] ? 1.f : 0.f)) * 1.6f * dt;
    }
    m_yaw   = wrapAngle(m_yaw);
    m_pitch = std::clamp(m_pitch, -1.55f, 1.55f);

    // wanted direction
    const V3 fwdFlat{std::sin(m_yaw), 0, -std::cos(m_yaw)};
    const V3 right{std::cos(m_yaw), 0, std::sin(m_yaw)};
    float    f = 0, r = 0;
    if (!m_typing) {
        f = (m_keys[K_W] ? 1.f : 0.f) - (m_keys[K_S] ? 1.f : 0.f);
        r = (m_keys[K_D] ? 1.f : 0.f) - (m_keys[K_A] ? 1.f : 0.f);
    }
    V3 wish = fwdFlat * f + right * r;
    if (m_scriptWalk > 0.f) {
        m_scriptWalk -= dt;
        wish += fwdFlat * m_scriptDir.z + right * m_scriptDir.x;
    }
    if (length(wish) > 1e-4f)
        wish = normalize(wish);

    const bool jump        = (!m_typing && m_keys[K_SPACE]) || m_scriptJump;
    const bool crouchKey   = !m_typing && (m_keys[K_LCTRL] || m_keys[K_C]);
    const bool sprint      = !m_typing && m_keys[K_LSHIFT];
    m_scriptJump           = false;

    if (m_fly) {
        V3 v = forwardFrom(m_yaw, m_pitch) * f + right * r;
        if (length(v) > 1e-4f)
            v = normalize(v);
        v = v * (FLY_SPEED * (sprint ? 2.f : 1.f));
        v.y += ((!m_typing && m_keys[K_SPACE]) ? FLY_SPEED : 0.f) - (crouchKey ? FLY_SPEED : 0.f);
        m_vel = lerp(m_vel, v, std::min(1.f, dt * 12.f));
        m_crouched = false;
    } else {
        // crouching, standing up only when there is room
        if (crouchKey)
            m_crouched = true;
        else if (m_crouched && !overlaps(m_feet, HEIGHT))
            m_crouched = false;

        const float speed = m_crouched ? CROUCH_SPEED : sprint ? SPRINT_SPEED : WALK_SPEED;
        const float accel = m_onGround ? 40.f : 10.f;
        const V3    target = wish * speed;
        V3          horiz{m_vel.x, 0, m_vel.z};
        V3          diff   = target - horiz;
        const float dl     = length(diff);
        const float maxD   = accel * dt;
        if (dl > maxD)
            diff = diff * (maxD / dl);
        m_vel.x += diff.x;
        m_vel.z += diff.z;

        m_vel.y -= GRAVITY * dt;
        m_vel.y = std::max(m_vel.y, -40.f);
        if (jump && m_onGround) {
            m_vel.y    = JUMP_SPEED;
            m_onGround = false;
        }
    }

    bool blocked = false;
    moveAxis(0, m_vel.x * dt, blocked);
    if (blocked)
        m_vel.x = 0;
    moveAxis(2, m_vel.z * dt, blocked);
    if (blocked)
        m_vel.z = 0;
    const float vy = m_vel.y;
    moveAxis(1, vy * dt, blocked);
    if (blocked) {
        m_onGround = vy < 0;
        m_vel.y    = 0;
    } else if (!m_fly)
        m_onGround = false;

    // fell out somehow
    const float floorY = m_world.model ? m_world.bounds.min.y : 0.f;
    if (m_feet.y < floorY - 10.f || !std::isfinite(m_feet.x + m_feet.y + m_feet.z))
        resetPlayer();

    const float eyeTarget = m_crouched ? EYE_CROUCH : EYE;
    m_eyeHeight += (eyeTarget - m_eyeHeight) * std::min(1.f, dt * 14.f);
}

// ---------------------------------------------------------------- aiming

void CDesktop3D::aim() {
    m_aimed = -1;
    m_aimSurface.reset();

    const V3 eye = m_camera.eye;
    const V3 dir = forwardFrom(m_camera.yaw, m_camera.pitch);

    float    best = 1e30f;
    Vector2D bestLocal;
    for (size_t i = 0; i < m_panels.size(); ++i) {
        const auto& p = m_panels[i];
        if (p.alpha < 0.05f || p.held)
            continue;
        float    t = 0;
        Vector2D local;
        if (!p.pose.intersect(eye, dir, t, local))
            continue;
        const CBox& c = p.clip;
        if (local.x < c.x || local.y < c.y || local.x > c.x + c.w || local.y > c.y + c.h)
            continue;
        // later panels are on top when at the same depth
        if (t <= best + 1e-4f) {
            best      = t;
            m_aimed   = (int)i;
            bestLocal = local;
        }
    }
    if (m_aimed < 0)
        return;

    if (SRayHit hit; m_world.collision.raycast(eye, dir, best - 1e-3f, hit)) {
        m_aimed = -1;
        return;
    }

    const auto& p   = m_panels[m_aimed];
    m_aimPanelLocal = bestLocal;
    if (p.hitRoot) {
        const auto [surf, local] = p.hitRoot->at(m_aimPanelLocal * p.hitScale - p.hitOffset, true);
        if (surf) {
            m_aimSurface = surf;
            m_aimLocal   = local;
        }
    }
}

void CDesktop3D::updatePointer() {
    SP<CWLSurfaceResource> surf;
    Vector2D               local;

    if (m_drag.surface) {
        // implicit grab: keep sending to the pressed surface, relative to its panel plane
        surf = m_drag.surface.lock();
        local = m_lastSentLocal;
        if (surf) {
            for (const auto& p : m_panels) {
                if (p.key != m_drag.panel)
                    continue;
                float    t = 0;
                Vector2D panelLocal;
                if (p.pose.intersect(m_camera.eye, forwardFrom(m_camera.yaw, m_camera.pitch), t, panelLocal, false))
                    local = panelLocal * p.hitScale - p.hitOffset - m_drag.offset;
                break;
            }
        } else
            m_drag = {};
    } else if (m_aimed >= 0) {
        surf  = m_aimSurface.lock();
        local = m_aimLocal;
    }

    const auto current = g_pSeatManager->m_state.pointerFocus.lock();
    if (surf != current) {
        g_pSeatManager->setPointerFocus(surf, local);
        m_lastSentLocal = local;
        if (surf)
            g_pSeatManager->sendPointerFrame();
        return;
    }

    if (surf && local != m_lastSentLocal) {
        g_pSeatManager->sendPointerMotion(nowMs(), local);
        g_pSeatManager->sendPointerFrame();
        m_lastSentLocal = local;
    }
}

// ------------------------------------------------------------------ input

bool CDesktop3D::onRelativeMotion(const Vector2D& delta) {
    if (m_mode == MODE_OFF)
        return false;
    if (m_mode == MODE_ACTIVE && m_menu.open())
        m_menu.move((float)delta.x, (float)delta.y);
    else if (m_mode == MODE_ACTIVE)
        m_look += delta;
    return true;
}

bool CDesktop3D::onAbsoluteMotion(const Vector2D& abs) {
    if (m_mode == MODE_OFF)
        return false;
    // absolute devices (tablets, nested sessions): turn by how far it moved
    if (m_mode == MODE_ACTIVE && m_lastAbs.x >= 0) {
        const auto mon = m_monitor.lock();
        const Vector2D size = mon ? mon->m_size : Vector2D{1920, 1080};
        const Vector2D d    = (abs - m_lastAbs) * size;
        if (m_menu.open())
            m_menu.move((float)d.x, (float)d.y);
        else
            m_look += d;
    }
    m_lastAbs = abs;
    return true;
}

void CDesktop3D::onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
    const bool pressed = e.state == WL_KEYBOARD_KEY_STATE_PRESSED;
    const uint32_t k   = e.keycode;

    if (!pressed) {
        if (m_consumed.erase(k)) {
            info.cancelled = true;
            if (k < m_keys.size())
                m_keys[k] = false;
        }
        return;
    }

    if (m_mode == MODE_OFF || m_mode == MODE_EXITING)
        return;

    if (g_pSessionLockManager->isSessionLocked()) {
        exitNow();
        return;
    }

    const uint32_t mods = g_pInputManager->getModsFromAllKBs();
    const bool     meta = mods & HL_MODIFIER_META;

    if (m_typing) {
        if (k == K_ESC && meta) {
            setTyping(false);
            info.cancelled = true;
            m_consumed.insert(k);
        }
        return;
    }

    // keep compositor shortcuts working
    if (meta || ((mods & HL_MODIFIER_CTRL) && (mods & HL_MODIFIER_ALT)) || k == K_LMETA || k == K_RMETA || k == K_LALT || k == K_RALT)
        return;

    info.cancelled = true;
    m_consumed.insert(k);

    if (m_mode != MODE_ACTIVE)
        return;

    // VRChat's gestures: left shift and F1-F8 for the left hand, right shift for the right one (neither: both)
    if (k >= K_F1 && k <= K_F8) {
        if (m_avatar) {
            const bool l = m_keys[K_LSHIFT], r = m_keys[K_RSHIFT];
            if (l || !r)
                m_anim.setGesture(0, (int)(k - K_F1));
            if (r || !l)
                m_anim.setGesture(1, (int)(k - K_F1));
        }
        return;
    }
    if (k == K_TAB) {
        if (m_menu.open())
            m_menu.hide();
        else
            m_menu.show();
        return;
    }
    if (menuKey(m_menu, k, [this](const std::optional<SMenuItem>& it) { menuPick(it); }))
        return; // (else walking still works)

    switch (k) {
        case K_ESC:
            if (m_hold.key)
                cancelHold();
            else
                exit();
            return;
        case K_G:
            if (m_hold.key)
                place();
            else
                grab();
            return;
        case K_X:
            if (m_hold.key) {
                const uintptr_t key = m_hold.key;
                m_hold              = {};
                returnToWall(key);
            } else if (m_aimed >= 0 && m_panels[m_aimed].kind != PANEL_LAYER) {
                const auto w = m_panels[m_aimed].window.lock();
                if (w)
                    returnToWall(reinterpret_cast<uintptr_t>(w.get()));
            }
            return;
        case K_E:
        case K_ENTER: setTyping(true); return;
        case K_R: resetPlayer(); return;
        case K_F:
            m_fly = !m_fly;
            m_vel = {};
            return;
        case K_V:
            if (const std::string r = setView(!m_thirdPerson); r.starts_with("error: "))
                notify(r.substr(7), true);
            return;
        default: break;
    }

    if (k < m_keys.size())
        m_keys[k] = true;
}

void CDesktop3D::onButton(uint32_t timeMs, uint32_t button, bool pressed, Event::SCallbackInfo* info) {
    if (!pressed && m_sentButtons.contains(button)) {
        // finish a click the client saw start, whatever the mode is now
        if (info)
            info->cancelled = true;
        m_sentButtons.erase(button);
        g_pSeatManager->sendPointerButton(timeMs, button, WL_POINTER_BUTTON_STATE_RELEASED);
        g_pSeatManager->sendPointerFrame();
        if (m_sentButtons.empty())
            m_drag = {};
        return;
    }

    if (m_mode == MODE_OFF)
        return;
    if (info)
        info->cancelled = true;

    // the Action Menu: a click picks, the right button goes back, the middle one closes it
    if (m_mode == MODE_ACTIVE && m_menu.open()) {
        if (pressed)
            menuButton(m_menu, button, [this](const std::optional<SMenuItem>& it) { menuPick(it); });
        return;
    }

    if (pressed && m_mode == MODE_ACTIVE && m_hold.key) {
        if (button == BTN_LEFT_)
            place();
        else if (button == BTN_RIGHT_)
            cancelHold();
        return;
    }

    if (!pressed || m_mode != MODE_ACTIVE || m_aimed < 0)
        return;

    const auto  surf = m_aimSurface.lock();
    const auto& p    = m_panels[m_aimed];

    // clicking outside of an open menu closes it, like on the 2D desktop
    if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(surf)) {
        g_pSeatManager->setGrab(nullptr);
        return;
    }

    if (p.kind == PANEL_WINDOW) {
        const auto w = p.window.lock();
        if (w && w != Desktop::focusState()->window())
            Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK, surf);
    }

    if (!surf)
        return;

    updatePointer(); // focus may have moved, make sure the client knows where we are
    g_pSeatManager->sendPointerButton(timeMs, button, WL_POINTER_BUTTON_STATE_PRESSED);
    g_pSeatManager->sendPointerFrame();
    m_sentButtons.insert(button);

    if (!m_drag.surface) {
        m_drag.panel   = p.key;
        m_drag.surface = surf;
        m_drag.offset  = (m_aimPanelLocal * p.hitScale - p.hitOffset) - m_aimLocal;
    }
}

void CDesktop3D::onAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
    if (m_mode == MODE_OFF)
        return;
    info.cancelled = true;

    // the Action Menu: the wheel goes round it, a notch an item
    if (m_mode == MODE_ACTIVE && m_menu.open()) {
        if (e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            menuWheel(m_menu, m_menuWheel, e.deltaDiscrete != 0 ? e.deltaDiscrete / 120.f : (float)e.delta / 15.f);
        return;
    }

    // carrying a window: the wheel pushes it away / pulls it closer, with ctrl it resizes
    if (m_mode == MODE_ACTIVE && m_hold.key && e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        const float notches = e.deltaDiscrete != 0 ? e.deltaDiscrete / 120.f : (float)e.delta / 15.f;
        if (g_pInputManager->getModsFromAllKBs() & HL_MODIFIER_CTRL)
            m_hold.scaleMul = std::clamp(m_hold.scaleMul * std::pow(0.92f, notches), 0.2f, 5.f);
        else
            m_hold.dist = std::clamp(m_hold.dist * std::pow(0.9f, notches), 0.6f, 12.f);
        return;
    }

    // third person, not pointing at anything that scrolls: the wheel zooms
    if (m_mode == MODE_ACTIVE && m_thirdPerson && m_aimSurface.expired() && e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        const float notches = e.deltaDiscrete != 0 ? e.deltaDiscrete / 120.f : (float)e.delta / 15.f;
        m_camDist           = std::clamp(m_camDist * std::pow(1.12f, notches), 0.8f, 10.f);
        return;
    }

    if (m_mode != MODE_ACTIVE || m_aimSurface.expired())
        return;

    static auto PSCROLL   = CConfigValue<Config::FLOAT>("input:scroll_factor");
    static auto PTPSCROLL = CConfigValue<Config::FLOAT>("input:touchpad:scroll_factor");
    const double factor   = e.source == WL_POINTER_AXIS_SOURCE_FINGER ? *PTPSCROLL : *PSCROLL;

    // deltaDiscrete is already in 1/120ths of a notch (hi-res wheels send
    // fractions); clients without v120 support only get whole notches
    const int32_t value120 = std::round(factor * e.deltaDiscrete);
    int32_t       discrete = 0;
    if (value120 != 0 && e.axis < m_scrollAcc.size()) {
        auto& acc = m_scrollAcc[e.axis];
        if (std::signbit(acc) != std::signbit((double)value120))
            acc = 0;
        acc += value120;
        discrete = (int32_t)(acc / 120.0);
        acc -= discrete * 120.0;
    }

    g_pSeatManager->sendPointerAxis(e.timeMs, e.axis, e.delta * factor, discrete, value120, e.source, e.relativeDirection);
    g_pSeatManager->sendPointerFrame();
}

// ----------------------------------------------------------------- drawing

std::vector<UP<IPassElement>> CDesktop3D::drawFrame() {
    std::vector<UP<IPassElement>> out;
    const auto                    mon = m_monitor.lock();
    if (!mon || m_mode == MODE_OFF || m_rendererFailed)
        return out;

    const int w = (int)std::round(mon->m_transformedSize.x);
    const int h = (int)std::round(mon->m_transformedSize.y);
    if (w < 1 || h < 1)
        return out;

    if (m_rendererDirty || !m_renderer.ready()) {
        m_rendererDirty = false;
        bool ok = m_renderer.init(m_world);
        if (!ok && m_world.model) {
            // most likely out of video memory for the map's textures
            notify("couldn't put map " + m_world.name + " on the GPU, back to the courtyard", true);
            m_mapPath.clear();
            m_world    = {};
            m_worldFor = {-1, -1};
            buildWorldFor(mon->m_size);
            resetPlayer();
            ok = m_renderer.init(m_world);
        }
        if (!ok) {
            m_rendererFailed = true;
            notify("couldn't start the 3D renderer, check the Hyprland log", true);
            exitNow();
            return out;
        }
    }

    if (!m_outTex || m_outSize != Vector2D{(double)w, (double)h}) {
        m_outTex = g_pHyprRenderer->createTexture(true);
        m_outTex->allocate({(double)w, (double)h});
        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glBindTexture(GL_TEXTURE_2D, m_outTex->m_texID);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        m_outTex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        m_outTex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        m_outTex->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        m_outTex->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, prev);
        m_outTex->m_type   = Render::TEXTURE_RGBX;
        m_outTex->m_opaque = true;
        m_outSize          = {(double)w, (double)h};
    }

    SFrameParams f;
    f.width     = w;
    f.height    = h;
    f.view      = m_view;
    f.proj      = m_proj;
    f.eye       = m_camera.eye;
    f.time      = m_time;
    f.monScale  = mon->m_scale;
    f.panels    = &m_panels;
    f.aimed     = m_mode == MODE_ACTIVE && !m_menu.visible() ? m_aimed : -1;
    f.crosshair = !m_menu.visible();
    f.typing    = m_typing;
    f.hudAlpha  = smoothstep01(std::clamp((m_t - 0.6f) / 0.4f, 0.f, 1.f));
    f.exposure  = m_exposure;
    if (m_avatar) {
        f.avatar.model     = m_avatar;
        f.avatar.joints    = &m_anim.joints();
        f.avatar.morphs    = &m_anim.morphWeights();
        f.avatar.materials = m_anim.materials();
        f.avatar.shown     = &m_anim.partsShown();
        f.avatar.batchMaterials = m_anim.batchMaterials();
        f.avatar.transform = avatarTransform();
        // not from inside its head
        f.avatar.visible = m_thirdPerson && (m_mode != MODE_ACTIVE || m_camBoom > 0.35f);
        f.avatar.sky     = m_avatarLight.skyAvg;
        f.avatar.bounce  = m_avatarLight.bounceAvg;
    }

    f.menu = m_menu.hud();
    f.menu.alpha *= f.hudAlpha;
    if (m_mic.on()) { // while it listens, a badge says so
        const float scale = (float)mon->m_scale;
        if (scale != m_badgeScale || m_badge.empty()) {
            drawBadge(m_badge, m_badgeW, m_badgeH, "lip sync: listening", scale);
            m_badgeScale = scale;
            ++m_badgeSerial;
        }
        const float margin = 12.f * scale;
        f.badge            = {.pixels = &m_badge, .w = m_badgeW, .h = m_badgeH, .serial = m_badgeSerial, .x = f.width - margin - m_badgeW / 2.f,
                              .y = margin + m_badgeH / 2.f, .scale = 1, .alpha = 1};
    }

    m_renderer.render(f, m_outTex->m_texID);

    CTexPassElement::SRenderData data;
    data.tex = m_outTex;
    data.box = {0, 0, mon->m_transformedSize.x, mon->m_transformedSize.y};
    data.a   = 1.f;
    out.emplace_back(makeUnique<CTexPassElement>(std::move(data)));
    return out;
}

// ------------------------------------------------------------ action menu

// what's picked in the Action Menu
// ------------------------------------------------------------------ lip sync

std::string CDesktop3D::setLipSync(bool on) {
    if (on == m_lipsync)
        return lipSyncStatus();
    if (on && !CMicrophone::available()) {
        std::string error;
        m_mic.start(error);
        notify("lip sync: " + error, true);
        return "error: " + error;
    }
    m_lipsync = on;
    notify(on ? "lip sync on: the microphone moves the avatar's mouth while you're in 3D; nothing it hears is kept or sent" : "lip sync off");
    lipSync();
    return lipSyncStatus();
}

void CDesktop3D::lipSync() {
    const bool want = m_lipsync && m_mode != MODE_OFF && m_avatar;
    if (want && !m_mic.on()) {
        std::string error;
        if (!m_mic.start(error)) {
            m_lipsync = false;
            notify("lip sync: " + error, true);
            return;
        }
        m_lip.reset();
    } else if (!want && m_mic.on()) {
        m_mic.stop();
        m_lip.reset();
        m_anim.setVisemes({});
        m_badge.clear();
    }
    if (!m_mic.on())
        return;
    m_micSamples.clear();
    int rate = 0;
    m_mic.read(m_micSamples, rate);
    m_lip.feed(m_micSamples.data(), m_micSamples.size(), rate);
    m_anim.setVisemes(m_lip.visemes());
}

std::string CDesktop3D::lipSyncStatus() const {
    const auto& v = m_lip.visemes();
    return std::format(R"({{"on": {}, "listening": {}, "microphone": {}, "level": {:.1f}, "formants": [{:.0f}, {:.0f}], "visemes": {{"aa": {:.2f}, "ih": {:.2f}, "ou": {:.2f}, "ee": {:.2f}, "oh": {:.2f}}}}})",
                       m_lipsync, m_mic.on(), CMicrophone::available(), m_lip.level(), m_lip.f1(), m_lip.f2(), v[0], v[1], v[2], v[3], v[4]);
}

std::string CDesktop3D::menuAction(const SMenuItem& it) {
    switch (it.action) {
        case MA_VIEW: return setView(!m_thirdPerson);
        case MA_FLY:
            m_fly = !m_fly;
            m_vel = {};
            return m_fly ? "flying" : "walking";
        case MA_RESPAWN: resetPlayer(); return "ok";
        case MA_LIPSYNC: return setLipSync(!m_lipsync);
        default: break;
    }
    m_ctl.loading = m_avatarLoader.busy();
    const std::string r = m_ctl.action(it);
    return r.empty() ? "error: nothing to do" : r;
}

void CDesktop3D::menuPick(const std::optional<SMenuItem>& item) {
    if (!item)
        return; // a page, or back
    if (const std::string r = menuAction(*item); r.starts_with("error: "))
        notify(r.substr(7), true);
}

// menu [open [page]|close|toggle|back|pick [n]|move dx dy|scroll n]
std::string CDesktop3D::menuCommand(const std::vector<std::string>& args) {
    if (args.empty())
        return menuStatus(m_menu);
    if (m_mode != MODE_ACTIVE)
        return "error: not in 3D";
    if (args[0] == "open" || (args[0] == "toggle" && !m_menu.open()))
        setTyping(false);
    return h3d::menuCommand(m_menu, args, [this](const SMenuItem& it) { return menuAction(it); });
}

std::string CDesktop3D::menuDispatch(const std::string& arg) {
    std::istringstream       in(arg);
    std::vector<std::string> words;
    for (std::string w; in >> w;)
        words.push_back(w);
    static constexpr std::string_view VERBS[] = {"open", "close", "toggle", "back", "pick", "move", "scroll"};
    if (words.empty())
        words = {"toggle"};
    else if (std::ranges::find(VERBS, words[0]) == std::end(VERBS))
        words.insert(words.begin(), "open"); // a page
    return menuCommand(words);
}

// ---------------------------------------------------------------- hyprctl

std::string CDesktop3D::status() {
    std::string aimed = "null";
    if (m_aimed >= 0 && m_aimed < (int)m_panels.size()) {
        const auto& p     = m_panels[m_aimed];
        const auto  w     = p.window.lock();
        const char* kinds[] = {"layer", "window", "popup"};
        aimed = std::format(R"({{"kind": "{}", "title": "{}", "class": "{}", "local": [{:.1f}, {:.1f}], "surface": {}}})", kinds[p.kind], jsonEscape(w ? w->m_title : ""),
                            jsonEscape(w ? w->m_class : ""), m_aimPanelLocal.x, m_aimPanelLocal.y, m_aimSurface.expired() ? "false" : "true");
    }
    const char* modes[] = {"off", "entering", "active", "exiting"};
    return std::format(
        R"({{"mode": "{}", "view": "{}", "typing": {}, "fly": {}, "feet": [{:.3f}, {:.3f}, {:.3f}], "eye": [{:.3f}, {:.3f}, {:.3f}], "yaw": {:.2f}, "pitch": {:.2f}, "onGround": {}, "panels": {}, "aimed": {}, "fps": {:.1f}, "frames": {}, "minDt": {:.5f}, "sens": {}, "placed": {}, "holding": {}, "world": "{}", "map": "{}", "mapLoading": {}, "avatar": "{}", "avatarLoading": {}, "anim": "{}", "exposure": {:.2f}, "light": {:.3f}, "menu": "{}", "hooks": {{"motion": {}, "warp": {}, "cursor": {}}}}})",
        modes[m_mode], m_thirdPerson ? "third" : "first", m_typing, m_fly, m_feet.x, m_feet.y, m_feet.z, m_camera.eye.x, m_camera.eye.y, m_camera.eye.z, m_yaw * 180.f / F_PI, m_pitch * 180.f / F_PI, m_onGround,
        m_panels.size(), aimed, m_fps, m_frames, m_minDt, m_sens, m_placements.size(), m_hold.key != 0, jsonEscape(m_world.name), jsonEscape(m_mapPath), m_mapLoader.busy(),
        jsonEscape(m_avatarPath), m_avatarLoader.busy(), jsonEscape(m_anim.playing()), m_exposure, m_lightAvg, m_menu.open() ? jsonEscape(m_menu.path()) : "", m_hookMoved != nullptr, m_hookWarp != nullptr, m_hookCursor != nullptr);
}

std::string CDesktop3D::hyprctl(const std::string& request) {
    std::istringstream       in(request);
    std::vector<std::string> args;
    for (std::string s; in >> s;)
        args.push_back(s);
    if (!args.empty())
        args.erase(args.begin()); // "hypr3d"

    const std::string cmd = args.empty() ? "status" : args[0];
    auto              num = [&](size_t i, float def) {
        try {
            return i < args.size() ? std::stof(args[i]) : def;
        } catch (...) { return def; }
    };

    if (cmd == "status")
        return status();
    if (cmd == "toggle") {
        toggle();
        return "ok";
    }
    if (cmd == "on")
        return enter() ? "ok" : "error: can't enter 3D right now";
    if (cmd == "off") {
        exit(args.size() > 1 && args[1] == "now");
        return "ok";
    }
    if (cmd == "type") {
        setTyping(args.size() > 1 ? args[1] != "off" : !m_typing);
        return m_typing ? "typing" : "walking";
    }
    if (cmd == "look") { // raw mouse counts
        m_look += Vector2D{num(1, 0), num(2, 0)};
        return "ok";
    }
    if (cmd == "turn") { // absolute, degrees
        m_yaw   = wrapAngle(num(1, 0) * F_PI / 180.f);
        m_pitch = std::clamp(num(2, 0) * F_PI / 180.f, -1.55f, 1.55f);
        return "ok";
    }
    if (cmd == "tp") {
        m_feet = {num(1, m_feet.x), num(2, m_feet.y), num(3, m_feet.z)};
        m_vel  = {};
        return "ok";
    }
    if (cmd == "walk") { // walk <seconds> [forward|back|left|right]
        m_scriptWalk        = num(1, 1);
        const std::string d = args.size() > 2 ? args[2] : "forward";
        m_scriptDir         = d == "back" ? V3{0, 0, -1} : d == "left" ? V3{-1, 0, 0} : d == "right" ? V3{1, 0, 0} : V3{0, 0, 1};
        return "ok";
    }
    if (cmd == "jump") {
        m_scriptJump = true;
        return "ok";
    }
    if (cmd == "fly") {
        m_fly = !m_fly;
        m_vel = {};
        return m_fly ? "flying" : "walking";
    }
    if (cmd == "click") { // click [left|right|middle]
        const std::string b      = args.size() > 1 ? args[1] : "left";
        const uint32_t    button = b == "right" ? BTN_RIGHT_ : b == "middle" ? BTN_MIDDLE_ : BTN_LEFT_;
        onButton(nowMs(), button, true, nullptr);
        onButton(nowMs(), button, false, nullptr);
        return "ok";
    }
    if (cmd == "grab") { // pick up (or put down) the window under the crosshair, like G
        if (m_mode != MODE_ACTIVE)
            return "error: not in 3D";
        if (m_hold.key) {
            place();
            return "placed";
        }
        grab();
        return m_hold.key ? "holding" : "error: nothing to grab";
    }
    if (cmd == "place") {
        if (!m_hold.key)
            return "error: not holding anything";
        place();
        return "placed";
    }
    if (cmd == "hold") { // hold <distance> [scale]: while holding, set how far away and how big
        if (!m_hold.key)
            return "error: not holding anything";
        m_hold.dist     = std::clamp(num(1, m_hold.dist), 0.6f, 12.f);
        m_hold.scaleMul = std::clamp(num(2, m_hold.scaleMul), 0.2f, 5.f);
        return "ok";
    }
    if (cmd == "reset-windows") { // everything back on the desktop wall
        m_hold = {};
        for (auto& [key, pl] : m_placements)
            pl.returning = true;
        return "ok";
    }
    // everything after the command word (and more), so paths can have spaces
    auto afterWords = [&](int words) {
        std::string rest = request;
        for (int word = 0; word < words; ++word) {
            const size_t a = rest.find_first_not_of(" \t");
            const size_t b = a == std::string::npos ? std::string::npos : rest.find_first_of(" \t", a);
            rest           = b == std::string::npos ? "" : rest.substr(b);
        }
        return rest;
    };
    auto pathArg = [&] { return unquote(afterWords(2)); };

    if (cmd == "map") { // map [path|none|reload|forget|scale <meters per unit>]
        const std::string rest = pathArg();
        const std::string sub  = args.size() > 1 ? args[1] : "";
        if (rest.empty())
            return std::format(R"({{"world": "{}", "map": "{}", "loading": {}, "scale": {}, "state": "{}"}})", jsonEscape(m_world.name), jsonEscape(m_mapPath),
                               m_mapLoader.busy(), m_mapScale, m_mapPath.empty() ? "" : jsonEscape(mapStatePath(m_mapPath)));
        if (sub == "none" || sub == "off" || sub == "courtyard")
            return requestMap("", 0);
        if (sub == "reload" || sub == "forget" || sub == "scale") {
            if (m_mapPath.empty())
                return "error: no map loaded";
            if (sub == "forget") { // drop the saved spawn and desktop, guess them again
                std::error_code ec;
                std::filesystem::remove(mapStatePath(m_mapPath), ec);
            }
            const float scale = sub == "scale" ? num(2, 0) : sub == "forget" ? (g_cfgMapScale ? g_cfgMapScale->value() : 0.f) : m_mapScale;
            return requestMap(m_mapPath, scale);
        }
        return requestMap(rest, g_cfgMapScale ? g_cfgMapScale->value() : 0.f);
    }
    // avatar [path|none|reload|height <meters>|expression [name [weight]|none]|gesture [left|right|both <gesture>]|parts [reset]|
    //         toggle <name> [on|off|reset]|shape <shape key> [weight|reset]|slider <name> [0..1|NN%|reset]|physics [on|off|toggle]|
    //         emote [name|number|file|folder [once|loop]|stop]]
    if (cmd == "avatar") {
        const std::string rest = pathArg();
        const std::string sub  = args.size() > 1 ? args[1] : "";
        if (rest.empty())
            return avatarStatus();
        if (sub == "lipsync") { // avatar lipsync [on|off|toggle]
            const std::string v = args.size() > 2 ? args[2] : "";
            if (v == "on" || v == "off" || v == "toggle") {
                if (const std::string r = setLipSync(v == "on" || (v == "toggle" && !m_lipsync)); r.starts_with("error"))
                    return r;
            } else if (!v.empty())
                return "error: avatar lipsync [on|off|toggle]";
            return lipSyncStatus();
        }
        m_ctl.loading       = m_avatarLoader.busy();
        m_ctl.emotesLoading = !m_emoteLoading.empty();
        if (const std::string r = m_ctl.command(args, afterWords(3), [this](const std::string& file, int loop) { return loadEmoteFile(file, loop); }); !r.empty())
            return r;
        if (sub == "none" || sub == "off")
            return requestAvatar("", 0);
        if (sub == "reload" || sub == "height") {
            if (m_avatarPath.empty())
                return "error: no avatar loaded";
            return requestAvatar(m_avatarPath, sub == "height" ? std::clamp(num(2, 0), 0.f, 20.f) : m_avatarHeight);
        }
        return requestAvatar(rest, g_cfgAvatarHeight ? g_cfgAvatarHeight->value() : 0.f);
    }
    if (cmd == "view") { // view [first|third|toggle] [distance] [side]
        const std::string v = args.size() > 1 ? args[1] : "";
        if (v.empty())
            return m_thirdPerson ? "third" : "first";
        const std::string r = setView(v == "third" || (v == "toggle" && !m_thirdPerson));
        if (m_thirdPerson) {
            m_camDist = std::clamp(num(2, m_camDist), 0.8f, 10.f);
            m_camSide = std::clamp(num(3, m_camSide), -1.f, 1.f);
        }
        return r;
    }
    if (cmd == "spawn") { // spawn [here]: go back to the spawn, or make where you stand the spawn
        if (args.size() > 1 && args[1] == "here") {
            if (m_mode != MODE_ACTIVE)
                return "error: not in 3D";
            m_world.spawn    = m_feet;
            m_world.spawnYaw = m_yaw;
            if (!m_mapPath.empty() && m_world.model && !saveMapState(m_mapPath, m_world, m_mapScale))
                return "error: couldn't save to " + mapStatePath(m_mapPath);
            return "ok";
        }
        resetPlayer();
        return "ok";
    }
    if (cmd == "desktop") { // desktop here [height in meters]: hang the desktop where the crosshair is
        if (args.size() < 2 || args[1] != "here")
            return std::format(R"({{"center": [{:.3f}, {:.3f}, {:.3f}], "normal": [{:.3f}, {:.3f}, {:.3f}], "height": {:.3f}}})", m_world.desktop.center.x, m_world.desktop.center.y,
                               m_world.desktop.center.z, m_world.desktop.normal.x, m_world.desktop.normal.y, m_world.desktop.normal.z, m_world.desktop.height);
        return placeDesktop(num(2, 0));
    }
    if (cmd == "menu")
        return menuCommand({args.begin() + 1, args.end()});
    if (cmd == "sens") {
        if (args.size() > 1)
            m_sens = std::clamp(num(1, m_sens), 0.00005f, 0.05f);
        return std::format("{}", m_sens);
    }
    return "usage: hyprctl hypr3d [status|toggle|on|off [now]|type [on|off]|look dx dy|turn yaw pitch|tp x y z|walk secs [forward|back|left|right]|jump|fly|click "
           "[left|right|middle]|sens [value]|grab|place|hold dist [scale]|reset-windows|map [path|none|reload|forget|scale s]|spawn [here]|desktop [here [height]]|"
           "avatar [path|none|reload|height m|expression [name [weight]|none]|gesture [left|right|both gesture]|parts [reset]|toggle name [on|off|reset]|"
           "shape name [weight|reset]|physics [on|off|toggle]|emote [name|number|file|folder [once|loop]|stop]]|view [first|third|toggle] [distance] [side]|"
           "menu [open [page]|close|toggle|back|pick [n]|move dx dy|scroll n]]";
}

// ---------------------------------------------------------------- lua / api

namespace {
    int luaToggle(lua_State*) {
        if (g_p3D)
            g_p3D->toggle();
        return 0;
    }
    int luaEnter(lua_State*) {
        if (g_p3D)
            g_p3D->enter();
        return 0;
    }
    int luaExit(lua_State*) {
        if (g_p3D)
            g_p3D->exit();
        return 0;
    }
    int luaType(lua_State*) {
        if (g_p3D)
            g_p3D->setTyping(true);
        return 0;
    }

    // Lua's own functions, from the Lua Hyprland runs; looked up when they're needed (a plugin that wants them
    // when it's loaded doesn't load at all where Hyprland has them built in)
    template <typename F>
    F luaFunction(const char* name) {
        return reinterpret_cast<F>(dlsym(RTLD_DEFAULT, name));
    }

    // hl.plugin.hypr3d.menu() toggles the Action Menu, menu("emotes") opens that page, menu("pick 2") and the like
    // do what hyprctl hypr3d menu does; it gives back what that says
    int luaMenu(lua_State* L) {
        using FType       = int (*)(lua_State*, int);
        using FToString   = const char* (*)(lua_State*, int, size_t*);
        using FPushString = const char* (*)(lua_State*, const char*, size_t);
        static const auto type       = luaFunction<FType>("lua_type");
        static const auto toString   = luaFunction<FToString>("lua_tolstring");
        static const auto pushString = luaFunction<FPushString>("lua_pushlstring");
        constexpr int     LUA_STRING = 4; // LUA_TSTRING
        if (!g_p3D)
            return 0;
        std::string arg;
        if (type && toString && type(L, 1) == LUA_STRING) {
            size_t n = 0;
            if (const char* s = toString(L, 1, &n))
                arg.assign(s, n);
        }
        const std::string r = g_p3D->menuDispatch(arg);
        if (!pushString)
            return 0;
        pushString(L, r.data(), r.size());
        return 1;
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        notify("built for a different Hyprland version, rebuild it with ./build.sh", true);
        throw std::runtime_error("[hypr3d] version mismatch");
    }

    // plugin { hypr3d { layer_spacing = ..., wallpaper = ... } }
    g_cfgSpacing = makeShared<Config::Values::CFloatValue>("plugin:hypr3d:layer_spacing", "meters between stacked desktop layers in 3D", 0.02f,
                                                          Config::Values::SFloatValueOptions{.min = 0.f, .max = 0.5f});
    g_cfgWallpaper = makeShared<Config::Values::CBoolValue>("plugin:hypr3d:wallpaper", "keep the wallpaper on the desktop wall in 3D", false);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgSpacing);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgWallpaper);
    // plugin { hypr3d { map = "~/maps/de_dust2.glb", map_scale = 0 } }
    g_cfgMap      = makeShared<Config::Values::CStringValue>("plugin:hypr3d:map", "glTF map to walk around in instead of the courtyard", "");
    g_cfgMapScale = makeShared<Config::Values::CFloatValue>("plugin:hypr3d:map_scale", "meters per map unit, 0 = guess", 0.f,
                                                            Config::Values::SFloatValueOptions{.min = 0.f, .max = 1000.f});
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgMap);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgMapScale);
    // plugin { hypr3d { avatar = "~/avatars/me.vrm", avatar_height = 0, avatar_physics = true } }
    g_cfgAvatar       = makeShared<Config::Values::CStringValue>("plugin:hypr3d:avatar", "glTF / GLB / VRM avatar, seen in third person (V)", "");
    g_cfgAvatarHeight = makeShared<Config::Values::CFloatValue>("plugin:hypr3d:avatar_height", "the avatar's height in meters, 0 = as it comes", 0.f,
                                                                Config::Values::SFloatValueOptions{.min = 0.f, .max = 20.f});
    g_cfgAvatarPhysics = makeShared<Config::Values::CBoolValue>("plugin:hypr3d:avatar_physics", "the avatar's hair, skirt and the like swing as it moves", true);
    // plugin { hypr3d { lipsync = false } }: off unless asked for; nothing heard is kept or sent
    g_cfgLipSync = makeShared<Config::Values::CBoolValue>("plugin:hypr3d:lipsync", "lip sync: the microphone moves the avatar's mouth while in 3D", false);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgLipSync);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatar);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatarHeight);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatarPhysics);
    g_cfgAvatarEmotes = makeShared<Config::Values::CStringValue>("plugin:hypr3d:avatar_emotes",
                                                                 "more emotes: VRM animations (.vrma) or glTF clips, files or folders separated by commas", "");
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatarEmotes);

    g_p3D = std::make_unique<CDesktop3D>();
    g_p3D->init();

    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "toggle", luaToggle);
    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "enter", luaEnter);
    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "exit", luaExit);
    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "type", luaType);
    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "menu", luaMenu);

    log("loaded");
    return {"hypr3d", "Walk around your desktop in first person", "hypr3d", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_p3D)
        g_p3D->shutdown();
    g_p3D.reset();
}
