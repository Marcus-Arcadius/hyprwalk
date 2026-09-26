// hypr3d: walk around your Hyprland desktop in first person.
//
// While 3D mode is on, a custom pass element covers the whole monitor. It
// draws a small procedural world plus every window/layer/popup as a textured
// panel hanging on its north wall, and hands the result back to Hyprland as a
// plain texture. Input is taken over: the mouse turns the camera, WASD walks,
// and clicks/scrolls go to whatever surface the crosshair points at. With an
// avatar loaded (plugin:hypr3d:avatar), V switches to a third person view of it,
// and Tab opens the Action Menu: its emotes, expressions, gestures and outfit.

#include "apps.hpp"
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
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/errorOverlay/Overlay.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/managers/PointerManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/notification/NotificationOverlay.hpp>
#include <hyprland/src/protocols/PointerConstraints.hpp>
#include <hyprland/src/protocols/RelativePointer.hpp>
#include <hyprland/src/protocols/core/DataDevice.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <aquamarine/backend/Backend.hpp>

#include <array>
#include <ctime>
#include <deque>
#include <mutex>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cmath>
#include <numbers>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace h3d {
    namespace {
        // the last lines logged, for hyprctl hypr3d log: Hyprland writes its log only with debug:disable_logs off
        constexpr size_t        LOG_LINES = 400;
        std::mutex              g_logMutex;
        std::deque<std::string> g_logLines;

        void remember(const char* level, const std::string& s) {
            const auto   now = std::chrono::system_clock::now();
            const time_t t   = std::chrono::system_clock::to_time_t(now);
            tm           local{};
            localtime_r(&t, &local);
            char when[16];
            std::strftime(when, sizeof(when), "%H:%M:%S", &local);
            const auto      ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
            std::lock_guard lock(g_logMutex);
            g_logLines.push_back(std::format("{}.{:03} {} {}", when, ms, level, s));
            if (g_logLines.size() > LOG_LINES)
                g_logLines.pop_front();
        }
    }

    void log(const std::string& s) {
        remember("INFO", s);
        Log::logger->log(Log::INFO, "[hypr3d] {}", s);
    }

    void notify(const std::string& s, bool error) {
        if (error) {
            remember("ERR", s);
            Log::logger->log(Log::ERR, "[hypr3d] {}", s); // INFO is hidden unless debug logs are on
        } else
            log(s);
        if (PHANDLE)
            HyprlandAPI::addNotification(PHANDLE, "[hypr3d] " + s, error ? CHyprColor{1.0, 0.35, 0.35, 1.0} : CHyprColor{0.45, 0.8, 1.0, 1.0}, error ? 8000 : 4000);
    }

    std::string logLines(size_t n) {
        std::lock_guard lock(g_logMutex);
        std::string     out;
        for (size_t i = g_logLines.size() > n ? g_logLines.size() - n : 0; i < g_logLines.size(); ++i)
            out += g_logLines[i] + "\n";
        return out;
    }
}

using namespace h3d;

namespace {
    PHLWINDOW parentOf(const PHLWINDOW& w); // (apps and windows, below)

    constexpr float F_PI            = std::numbers::pi_v<float>;
    constexpr float FOV_Y         = 70.f * F_PI / 180.f;
    constexpr float ENTER_TIME    = 0.8f;
    constexpr float EXIT_TIME     = 0.6f;
    constexpr float PLAY_TIME     = 0.35f; // the camera going to a window played, and back
    constexpr float PLAY_FILL     = 0.94f; // how much of the view it fills, the way it fits
    constexpr int   ICON_PX       = 96;    // the apps' pictures in the Action Menu (it scales them)

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
        K_Q      = 16,
        K_W      = 17,
        K_E      = 18,
        K_R      = 19,
        K_P      = 25,
        K_ENTER  = 28,
        K_LCTRL  = 29,
        K_A      = 30,
        K_S      = 31,
        K_D      = 32,
        K_F      = 33,
        K_G      = 34,
        K_H      = 35,
        K_LSHIFT = 42,
        K_X      = 45,
        K_C      = 46,
        K_V      = 47,
        K_B      = 48,
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
    uint64_t     pinned    = 0;        // follows the view in its top right corner (the order it was pinned in), 0 = not
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
    SP<Config::Values::CStringValue> g_cfgLipSyncGain;   // dB, or auto
    SP<Config::Values::CStringValue> g_cfgLipSyncSource; // the microphone, "" = the default one
    SP<Config::Values::CStringValue> g_cfgAvatarEmotes;
    SP<Config::Values::CStringValue> g_cfgApps;     // the Apps page's favourites
    SP<Config::Values::CStringValue> g_cfgAppRules; // where apps launched from 3D open
    SP<Config::Values::CFloatValue>  g_cfgPinSize;  // how much of the view's height a pinned window takes
    SP<Config::Values::CStringValue> g_cfgMonitor;  // the monitor 3D goes on, "" = the focused one

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

    // plugin:hypr3d:monitor as it's set, "" for the focused monitor
    std::string configuredMonitor() {
        const std::string v = g_cfgMonitor ? unquote(g_cfgMonitor->value()) : "";
        return v == "[[EMPTY]]" ? "" : v;
    }

    // a monitor by its name as hyprctl monitors lists it (DP-1), or by desc: and the start of its description, as
    // Hyprland's monitor rules take them; null when no monitor that's connected is
    PHLMONITOR monitorNamed(const std::string& name) {
        const std::string v = unquote(name);
        if (v.empty())
            return nullptr;
        for (const auto& m : g_pCompositor->m_monitors)
            if (m && m->m_output && m->matchesStaticSelector(v))
                return m;
        return nullptr;
    }

    // into 3D from another monitor: the keyboard focus doesn't stay with a window there, unseen (Super+Q would close
    // it); what's clicked, typed into or played in 3D gets it
    void unfocusOthers(const PHLMONITOR& mon) {
        if (const auto w = Desktop::focusState()->window(); w && w->m_monitor.lock() != mon)
            Desktop::focusState()->rawWindowFocus(nullptr, Desktop::FOCUS_REASON_OTHER);
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
    // the mouse and keyboard to the desktop on another monitor, the 3D view staying up on its own (Super+Esc), or
    // back into 3D
    std::string                   setAway(bool away);
    bool                          away() const {
        return m_away;
    }

    std::string                   hyprctl(const std::string& request);
    // the Action Menu: open [page], close, toggle, back, pick [n], move dx dy, scroll n; none = what it shows
    std::string                   menuCommand(const std::vector<std::string>& args);
    std::string                   menuDispatch(const std::string& arg); // nothing = toggle, a page = open it, else a command

    // P: the window under the crosshair gets every key, button and the mouse (a game), and the camera faces it
    std::string                   setPlay(bool on);
    bool                          playing() const {
        return m_play.on;
    }

    // hooks, return true when the event was eaten
    bool                          onRelativeMotion(const IPointer::SMotionEvent& e);
    bool                          onAbsoluteMotion(const Vector2D& abs);
    void                          onPointerFrame();
    bool                          holdsPointer() const { // in 3D, or going in or out, and the mouse isn't away on another monitor
        return m_mode != MODE_OFF && !m_away;
    }
    // drawn in 3D this frame: it gets presentation feedback from here, as presented, instead of Hyprland's
    // "discarded" for what the 3D view covers
    bool                          drawsSurface(const CWLSurfaceResource* s) const {
        return m_mode != MODE_OFF && m_drawnSurfaces.contains(s);
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
    float                                 m_updateMs = 0, m_renderMs = 0; // the plugin's own time a frame (averaged): its update, its drawing (the CPU's)

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
    std::string                   m_lipsyncSource;                  // the microphone asked for, "" = the default one
    std::string                   m_lipsyncSourceConfigured = "\n"; // the config's, last seen
    std::string                   m_lipsyncGainConfigured   = "\n"; // the same for lipsync_gain
    // what the microphone is doing, watched a few times a second: for the badge, one notification each time it
    // starts listening, and hyprctl
    struct {
        int         problem  = -1;    // eMicProblem, as the badge has it
        bool        notified = false; // this time listening
        std::chrono::steady_clock::time_point looked; // last
        float       broken = 0;       // seconds the stream has been broken, to open it again
        std::string error;            // what broke it last (until it's linked again)
        std::string source;           // the source it was linked to last (node.name)
    } m_micWatch;
    std::vector<uint32_t>         m_badge;
    std::string                   m_badgeText; // what m_badge says
    int                           m_badgeW = 0, m_badgeH = 0;
    CBox                          m_badgeBox; // where it was drawn last, output pixels (empty: it wasn't)
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
    struct {
        bool     negative = false;
        uint32_t axis = 0, timeMs = 0;
        uint32_t acc = 0; // 1/120ths of a notch not sent as a whole one yet
    } m_wheel;            // a high-resolution wheel's notches made up for the window aimed at, as Hyprland does
    bool                         m_axisFramePending = false; // a touchpad's scrolling sent, its frame not yet (the device's)
    // Away: the mouse and keyboard are the desktop's on another monitor (Super+Esc, or a keybind that moved the focus
    // and the cursor there), while the 3D view stays up on its own monitor; the cursor coming back onto that one
    // comes back into 3D. In 3D Hyprland's cursor stays on the 3D monitor, where nothing shows it
    bool                         m_away = false;
    bool                         m_awayTold = false;   // how to come back was said, this time in 3D
    std::optional<Vector2D>      m_desktopAt;          // where the cursor was last on another monitor (Super+Esc goes there)
    bool                         m_ownMove = false;    // the plugin's own input.mouse.move (a drag moved where you point)
    UP<SEventLoopDoLaterLock>    m_followLater;        // followCursor() after this frame

    // the Action Menu (Tab), like VRChat's: while it's open the mouse moves its cursor, not the camera
    CActionMenu m_menu{[this](const std::string& id) {
                           if (id == "apps" || id == "apps/all" || id == "windows" || id.starts_with("win:"))
                               return ownPage(id); // (the apps and the windows: Hyprland's)
                           const auto gain = m_lip.gainSetting();
                           return actionPage(id, {m_avatar.get(), &m_anim, m_avatarLoader.busy(), m_thirdPerson, m_fly, m_lipsync, CMicrophone::available(),
                                                  gain ? *gain : NAN, m_lip.gain()});
                       },
                       [this](const SMenuItem& it, float v, float v2) { // a slider's dial (a stick: both); lip sync's gain
                           if (it.action == MA_LIPSYNC_GAIN)
                               m_lip.setGain(v < 0.025f ? std::nullopt : std::optional<float>(std::round(v * MIC_GAIN_MAX)));
                           else
                               m_ctl.dial(it, v, v2);
                       }};
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
    // where the pointer is on the panels, for the app's cursor: the panel (key, 0 = none) and a point on it
    struct {
        uintptr_t panel = 0;
        Vector2D  local;
    } m_pointerAt;
    bool                                   m_cursorShown = false; // the app's cursor is drawn this frame
    std::vector<PHLMONITORREF>             m_cursorLocks; // monitors whose hardware cursor is off while in 3D
    std::unordered_set<const CWLSurfaceResource*> m_drawnSurfaces; // the panels' surfaces this frame
    Time::steady_tp                        m_frameTime;

    // play mode (P): a window (a game, mostly) gets every key and button, the wheel, and the mouse the way it wants
    // it: only relative motion while it locks the pointer, else a pointer that moves over it as over a monitor (kept
    // in its confinement when it has one). The camera leaves the player to face it, and comes back when it ends
    struct {
        bool         on = false;
        PHLWINDOWREF window;
        Vector2D     pointer;    // over it, window-local logical px (its popups' too)
        Vector2D     lastGlobal; // Hyprland's cursor as last seen: in 3D only a warp moves it (wp_pointer_warp_v1)
        float        t = 0;      // 0 = the player's own view .. 1 = facing the window
        bool         framed = false; // eye and rot are where the camera faces it
        bool         byFullscreen = false; // it went fullscreen, and leaving fullscreen ends it
        float        unfocused    = 0;     // seconds no window has had the keyboard (a dialog closed, its window next)
        V3           eye;
        Quat         rot;
    } m_play;

    SCamera                   m_camera;
    V3                        m_camFwd{0, 0, -1}, m_camUp{0, 1, 0}; // the view's axes this frame (in play mode turned with the window)
    M4                        m_view, m_proj;
    float                     m_depthMul = 0;

    // apps: the XDG desktop entries (read when a page or hyprctl wants them, and again after half a minute), what was
    // launched from 3D and hasn't opened its window yet, the rules for where they open, and where windows were put in
    // this world, by class (kept across sessions)
    std::vector<SAppEntry>                       m_apps;
    std::chrono::steady_clock::time_point        m_appsRead{};
    struct SLaunch {
        std::string                           token; // in its environment, as HYPR3D_LAUNCH: its windows are ours
        std::string                           what, cls; // cls: the class it's expected to have ("" = any)
        std::string                           steam;     // a Steam game's app id: only its window is ours
        int64_t                               pid = 0;
        std::chrono::steady_clock::time_point at;
    };
    std::vector<SLaunch>                         m_launches;
    uint64_t                                     m_launchCount = 0;
    std::string                                  m_rulesConfigured = "\n";
    std::vector<SAppRule>                        m_rules;
    std::unordered_map<std::string, SWindowSpot> m_spots;
    std::string                                  m_spotsFor = "\n"; // the map they're for ("" = the courtyard)
    uint64_t                                     m_pinCount = 0;
    size_t                                       m_iconNext = 0; // the next app whose icon the menu's pages load (a few a frame)
    WP<CWLSurfaceResource>                       m_dndAt;   // what a drag was last moved over, and where
    Vector2D                                     m_dndLocal;

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
    CFunctionHook*                   m_hookWheel  = nullptr;
    CFunctionHook*                   m_hookFrame  = nullptr;
    CFunctionHook*                   m_hookSoftCursor = nullptr;
    CFunctionHook*                   m_hookDiscard    = nullptr;
    // idle events of aquamarine's that let go of a removed headless output (holdOutput())
    std::vector<SP<std::function<void()>>> m_outputHolds;

    void                             holdOutput(const PHLMONITOR& mon);
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
    void                             watchMicrophone(); // what's wrong with it, if anything: the badge says so
    std::string                      lipSyncStatus() const;
    std::string                      setLipSyncGain(const std::string& v); // dB, or auto
    void                             setLipSyncSource(const std::string& v); // a microphone, "" = the default one
    std::vector<std::string>         emoteFiles() const; // the config's, then those added
    void                             loadEmoteFiles(std::vector<std::string> files);
    void                             applyEmotes(SEmoteResult&& res);
    std::string                      menuAction(const SMenuItem& item);
    SMenuPage                        ownPage(const std::string& id); // apps, apps/all, windows, win:ADDRESS
    const std::vector<SAppEntry>&    apps();
    std::string                      launch(const std::string& what);
    void                             onWindowOpen(const PHLWINDOW& w);
    void                             onFullscreen(const PHLWINDOW& w);
    bool                             placeInFront(const PHLWINDOW& w, const SAppRule& rule);
    void                             placeAt(const PHLWINDOW& w, const SWindowSpot& spot);
    void                             loadSpots();
    void                             rememberSpot(uintptr_t key);
    void                             forgetSpot(uintptr_t key);
    void                             restoreSpots();
    PHLWINDOW                        findWindow(const std::string& what) const; // an address (0x...), a class or a title
    std::string                      windowAction(const PHLWINDOW& w, eWindowAction a);
    std::string                      setPinned(const PHLWINDOW& w, bool on);
    std::string                      togglePin();
    std::string                      resizeReal(const PHLWINDOW& w, const Vector2D& size);
    std::string                      windowsStatus() const;
    void                             checkAppRules();
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
    void                             updatePointer(uint32_t timeMs = 0, bool frame = true);
    int                              windowPanel(const PHLWINDOW& w) const; // its panel's index, -1 = none
    void                             endPlay();
    bool                             playedWith(const SPanel& q, const PHLWINDOW& w) const;
    void                             updatePlay(float dt);
    void                             aimPlay();
    void                             clampPlayPointer();
    void                             playMotion(const IPointer::SMotionEvent& e);
    std::string                      playStatus() const;
    void                             lockCursors(bool lock);
    bool                             onMouseMove(const Vector2D& pos); // Hyprland's pointer move: false = let it go on
    bool                             cursorAway() const; // should the mouse be away, going by where the cursor is
    void                             followCursor();     // away or back, as cursorAway() says
    void                             goAway(bool refocus);
    void                             comeBack();
    std::optional<Vector2D>          desktopSpot() const; // where Super+Esc puts the cursor, none = no other monitor
    void                             addAppCursor();
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
    using FnMouseWheel = void (*)(void*, IPointer::SAxisEvent, SP<IPointer>);
    using FnFrame      = void (*)(void*);
    using FnSoftCursor = void (*)(void*, PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D>, bool);
    using FnDiscard    = void (*)(CSurfacePassElement*);

    CFunctionHook* g_moved  = nullptr;
    CFunctionHook* g_warp   = nullptr;
    CFunctionHook* g_cursor = nullptr;
    CFunctionHook* g_wheel  = nullptr;
    CFunctionHook* g_frame  = nullptr;
    CFunctionHook* g_softCursor = nullptr;
    CFunctionHook* g_discard    = nullptr;
    // the device of the wheel event being handled: CInputManager::onMouseWheel takes its own scroll factor first, and
    // the event bus's input.mouse.axis, which it emits, doesn't say
    WP<IPointer> g_wheelPointer;

    void           hkMouseMoved(void* self, IPointer::SMotionEvent e) {
        if (g_p3D && g_p3D->onRelativeMotion(e))
            return;
        ((FnMouseMoved)g_moved->m_original)(self, e);
    }

    // In 3D the pointer manager keeps the app's cursor image (the plugin draws it on the panel), with the hardware
    // cursor off: Hyprland's own software cursor isn't drawn on any monitor, but a cursor surface still gets its frames.
    // (With the mouse away on another monitor, Hyprland draws its cursor as ever)
    void hkSoftCursors(void* self, PHLMONITOR mon, const Time::steady_tp& now, CRegion& damage, std::optional<Vector2D> at, bool force) {
        if (g_p3D && g_p3D->holdsPointer()) {
            if (const auto surf = g_pPointerManager->currentCursorImage().surface.lock(); surf && surf->resource())
                surf->resource()->frame(now);
            return;
        }
        ((FnSoftCursor)g_softCursor->m_original)(self, mon, now, damage, at, force);
    }

    // a surface the 3D view covers is "discarded" by Hyprland's render pass (a frame callback and discarded
    // presentation feedback); one the plugin draws gets presented feedback from it instead, after the frame
    void hkDiscard(CSurfacePassElement* self) {
        if (g_p3D && self && g_p3D->drawsSurface(self->m_data.surface.get()))
            return;
        ((FnDiscard)g_discard->m_original)(self);
    }

    void hkMouseWarp(void* self, IPointer::SMotionAbsoluteEvent e) {
        if (g_p3D && g_p3D->onAbsoluteMotion(e.absolute))
            return;
        ((FnMouseWarp)g_warp->m_original)(self, e);
    }

    void hkMouseWheel(void* self, IPointer::SAxisEvent e, SP<IPointer> pointer) {
        g_wheelPointer = pointer;
        ((FnMouseWheel)g_wheel->m_original)(self, e, pointer);
        g_wheelPointer.reset();
    }

    void hkPointerFrame(void* self) {
        ((FnFrame)g_frame->m_original)(self);
        if (g_p3D)
            g_p3D->onPointerFrame();
    }

    // in 3D Hyprland's reasons to hide the cursor (a timeout, a key press) don't count: nothing shows it on a monitor
    // (hkSoftCursors), and the plugin draws the app's own on the panel it's over. cursor:invisible still hides it
    void hkEnsureCursor(void* self) {
        if (g_p3D && g_p3D->holdsPointer()) {
            static auto PINVISIBLE = CConfigValue<Config::INTEGER>("cursor:invisible");
            // (without the hook that keeps it off the monitors it stays hidden, and so does the app's)
            g_pHyprRenderer->setCursorHidden(*PINVISIBLE != 0 || !g_softCursor);
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
    m_hookWheel  = g_wheel  = hookByName("onMouseWheel", "CInputManager::onMouseWheel(", (void*)&hkMouseWheel);
    m_hookFrame  = g_frame  = hookByName("onPointerFrame", "CInputManager::onPointerFrame(", (void*)&hkPointerFrame);
    m_hookSoftCursor = g_softCursor = hookByName("renderSoftwareCursorsFor", "CPointerManager::renderSoftwareCursorsFor(", (void*)&hkSoftCursors);
    m_hookDiscard    = g_discard    = hookByName("discard", "CSurfacePassElement::discard(", (void*)&hkDiscard);

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
        g_pHyprRenderer->ensureCursorRenderingMode(); // (hkEnsureCursor)
        // redraw everything, without damageMonitor(): that would also schedule
        // an extra frame outside of the display's pacing
        mon->m_damage.damageEntire();
    }));

    m_listeners.emplace_back(ev.render.stage.listen([this](eRenderStage stage) {
        if (stage == RENDER_POST) {
            // what was drawn in 3D was presented: its frame callbacks and presentation feedback, as Hyprland gives
            // them for what it draws (hkDiscard kept its "discarded" ones back), at the monitor's own pace
            const auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
            if (m_mode == MODE_OFF || !mon || mon != m_monitor.lock())
                return;
            for (const auto& p : m_panels)
                for (const auto& s : p.surfaces)
                    if (const auto res = s.surface.lock())
                        res->presentFeedback(m_frameTime, mon);
            return;
        }
        if (stage != RENDER_LAST_MOMENT || m_mode == MODE_OFF)
            return;
        const auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!mon || mon != m_monitor.lock())
            return;
        if (g_pSessionLockManager->isSessionLocked()) {
            exitNow();
            return;
        }

        m_frameTime = Time::steadyNow();
        const auto t0 = std::chrono::steady_clock::now();
        update();
        m_updateMs = m_updateMs * 0.95f + 0.05f * std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
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

    m_listeners.emplace_back(ev.monitor.added.listen([this](const PHLMONITOR& mon) {
        if (m_mode != MODE_OFF && !m_away)
            lockCursors(true); // (its hardware cursor off too)
    }));

    m_listeners.emplace_back(ev.monitor.removed.listen([this](const PHLMONITOR& mon) {
        holdOutput(mon);
        if (m_mode != MODE_OFF && (!m_monitor.lock() || mon == m_monitor.lock()))
            exitNow();
    }));

    m_listeners.emplace_back(ev.input.keyboard.key.listen([this](const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) { onKey(e, info); }));

    // apps launched from 3D open in front of you (or where their class was put before); a fullscreen request plays
    m_listeners.emplace_back(ev.window.open.listen([this](const PHLWINDOW& w) { onWindowOpen(w); }));
    m_listeners.emplace_back(ev.window.fullscreen.listen([this](const PHLWINDOW& w) { onFullscreen(w); }));

    m_listeners.emplace_back(ev.input.mouse.button.listen([this](const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        onButton(e.timeMs, e.button, e.state == WL_POINTER_BUTTON_STATE_PRESSED, &info);
    }));

    m_listeners.emplace_back(ev.input.mouse.axis.listen([this](const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) { onAxis(e, info); }));

    // no pointer refocusing, focus-follows-mouse or cursor warps while in 3D; the cursor put on another monitor (a
    // keybind's focus) goes away to it, and while away, the cursor coming onto the 3D monitor comes back
    m_listeners.emplace_back(ev.input.mouse.move.listen([this](const Vector2D& pos, Event::SCallbackInfo& info) {
        if (onMouseMove(pos))
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
        checkAppRules();
    }));
    // a value set at run time comes with no reload (hyprctl keyword, hl.config() through hyprctl eval): look again
    // every second
    m_configTimer = wl_event_loop_add_timer(
        g_pCompositor->m_wlEventLoop,
        [](void* data) {
            auto* self = (CDesktop3D*)data;
            self->checkMapConfig();
            self->checkAvatarConfig();
            self->checkAppRules();
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

    // hypr3d:play plays the window under the crosshair, or stops (as P and Super+Esc do)
    HyprlandAPI::addDispatcherV2(PHANDLE, "hypr3d:play", [this](std::string) {
        const std::string r = setPlay(!m_play.on);
        return r.starts_with("error: ") ? SDispatchResult{.success = false, .error = r.substr(7)} : SDispatchResult{};
    });

    // hypr3d:menu toggles the Action Menu, hypr3d:menu <page> opens that page, or a hyprctl hypr3d menu command
    HyprlandAPI::addDispatcherV2(PHANDLE, "hypr3d:menu", [this](std::string arg) {
        const std::string r = menuDispatch(arg);
        return r.starts_with("error: ") ? SDispatchResult{.success = false, .error = r.substr(7)} : SDispatchResult{};
    });

    // hypr3d:away sends the mouse and keyboard to the desktop on another monitor, the 3D view staying up, or brings
    // them back (as Super+Esc does)
    HyprlandAPI::addDispatcherV2(PHANDLE, "hypr3d:away", [this](std::string) {
        const std::string r = setAway(!m_away);
        return r.starts_with("error: ") ? SDispatchResult{.success = false, .error = r.substr(7)} : SDispatchResult{};
    });

    checkMapConfig();
    checkAvatarConfig();
    checkAppRules();
}

void CDesktop3D::checkAppRules() {
    const std::string spec = g_cfgAppRules ? g_cfgAppRules->value() : "";
    if (spec == m_rulesConfigured)
        return;
    m_rulesConfigured = spec;
    std::string error;
    m_rules = parseAppRules(spec, error);
    if (!error.empty())
        notify(error, true);
}

void CDesktop3D::shutdown() {
    // Hyprland only clears the render pass when the next frame begins; an
    // element left over from the last frame would then run our destructor
    // after this library is gone
    g_pHyprRenderer->m_renderPass.removeAllOfType(C3D_PASS_NAME);

    m_followLater.reset();
    if (m_mode != MODE_OFF) {
        m_mode = MODE_OFF;
        restore();
    } else if (m_restoreLater) {
        m_restoreLater.reset();
        restore();
    }

    for (auto** h : {&m_hookMoved, &m_hookWarp, &m_hookCursor, &m_hookWheel, &m_hookFrame, &m_hookSoftCursor, &m_hookDiscard}) {
        if (*h)
            HyprlandAPI::removeFunctionHook(PHANDLE, *h);
        *h = nullptr;
    }
    g_moved = g_warp = g_cursor = g_wheel = g_frame = g_softCursor = g_discard = nullptr;
    g_wheelPointer.reset();

    m_listeners.clear();
    // (their idle events are this plugin's code: out of aquamarine's queue before it goes)
    if (const auto backend = g_pCompositor->m_aqBackend)
        for (const auto& h : m_outputHolds)
            backend->removeIdleEvent(h);
    m_outputHolds.clear();
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
    HyprlandAPI::removeDispatcher(PHANDLE, "hypr3d:play");

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
    // (like the others: hyprctl's until the config's changes)
    if (const std::string gain = g_cfgLipSyncGain ? g_cfgLipSyncGain->value() : "auto"; gain != m_lipsyncGainConfigured) {
        m_lipsyncGainConfigured = gain;
        if (const std::string r = setLipSyncGain(gain); r.starts_with("error"))
            notify(r + " (plugin:hypr3d:lipsync_gain)", true);
    }
    if (const std::string source = g_cfgLipSyncSource ? g_cfgLipSyncSource->value() : ""; source != m_lipsyncSourceConfigured) {
        m_lipsyncSourceConfigured = source;
        setLipSyncSource(source);
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

    // the monitor asked for, else plugin:hypr3d:monitor's, else the focused one
    if (const std::string want = configuredMonitor(); !mon && !want.empty()) {
        mon = monitorNamed(want);
        if (!mon)
            notify(std::format("plugin:hypr3d:monitor: no monitor {} is connected, so 3D goes on the focused one", want), true);
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
    m_away     = false;
    m_awayTold = false;
    m_desktopAt.reset();
    m_followLater.reset();

    // the mouse and keyboard come to the 3D monitor: the cursor, when it's on another one (Super+Esc takes it back
    // there), and the focus, so that what opens goes there
    if (const Vector2D cursor = g_pPointerManager->position(); g_pCompositor->getMonitorFromVector(cursor) != mon) {
        m_desktopAt = cursor;
        g_pCompositor->warpCursorTo(mon->middle(), true);
    }
    Desktop::focusState()->rawMonitorFocus(mon);
    unfocusOthers(mon);

    g_pHyprRenderer->m_directScanoutBlocked = true;
    lockCursors(true);
    g_pHyprRenderer->ensureCursorRenderingMode(); // (hkEnsureCursor: the app's cursor, drawn on the panels)
    g_pHyprRenderer->damageMonitor(mon);
    g_pCompositor->scheduleFrameForMonitor(mon);
    restoreSpots(); // (they fly there from the wall as 3D comes in)

    log("entering 3D on " + mon->m_name);
    return true;
}

// the hardware cursor off on every monitor while in 3D (the pointer manager keeps the app's cursor image for the
// panels; hkSoftCursors keeps the software one off the monitors)
void CDesktop3D::lockCursors(bool lock) {
    if (!lock) {
        for (const auto& m : m_cursorLocks)
            if (const auto mon = m.lock())
                g_pPointerManager->unlockSoftwareForMonitor(mon);
        m_cursorLocks.clear();
        return;
    }
    for (const auto& mon : g_pCompositor->m_monitors) {
        if (!mon || std::ranges::any_of(m_cursorLocks, [&](const auto& m) { return m.lock() == mon; }))
            continue;
        g_pPointerManager->lockSoftwareForMonitor(mon);
        m_cursorLocks.emplace_back(mon);
    }
}

// ------------------------------------------------------------ other monitors

// Hyprland's pointer move (the mouse's, or a warp's that simulates one); true keeps it from Hyprland. In 3D the cursor
// stays where it is and focuses nothing, unless a keybind put it and the focus on another monitor (movefocus,
// focusmonitor): then the mouse and keyboard go away to that one. Away, the other monitors are the desktop's as ever,
// and the cursor coming onto the 3D monitor brings them back into 3D
bool CDesktop3D::onMouseMove(const Vector2D& pos) {
    if (m_mode == MODE_OFF || m_ownMove)
        return false;
    const auto mon = m_monitor.lock();
    const auto at  = g_pCompositor->getMonitorFromVector(pos);
    if (m_away) {
        if (at != mon) {
            m_desktopAt = pos;
            return false;
        }
        if (m_mode != MODE_ACTIVE)
            return false; // (leaving 3D: it's the desktop's there in a moment)
        comeBack();
        return true;
    }
    if (mon && at && at != mon && m_mode == MODE_ACTIVE && !m_play.on && at == Desktop::focusState()->monitor()) {
        goAway(false); // (this move goes on: what's under the cursor there gets the pointer)
        m_desktopAt = pos;
        return false;
    }
    return true;
}

// should the mouse and keyboard be away, going by the cursor: while it's on another monitor, once that one has the
// focus too (in play mode only the game moves it, by a warp to itself: not away then); back on the 3D monitor
bool CDesktop3D::cursorAway() const {
    const auto mon = m_monitor.lock();
    const auto at  = g_pCompositor->getMonitorFromCursor();
    if (!mon || !at || at == mon)
        return false;
    return m_away || (!m_play.on && at == Desktop::focusState()->monitor());
}

// away or back as the cursor says, for what moves it without a move Hyprland tells of (a keybind's focus warping it to
// a monitor with no window, Hyprland's own clamping when a monitor goes)
void CDesktop3D::followCursor() {
    if (m_mode != MODE_ACTIVE)
        return;
    if (const bool away = cursorAway(); away && !m_away)
        goAway(true);
    else if (!away && m_away)
        comeBack();
}

void CDesktop3D::goAway(bool refocus) {
    const auto mon = m_monitor.lock();
    if (m_mode != MODE_ACTIVE || m_away || !mon)
        return;
    m_away = true;
    // what 3D had of the mouse and keyboard, let go: a window's buttons released to it before the pointer leaves it
    if (!m_sentButtons.empty()) {
        const uint32_t t = nowMs();
        for (uint32_t b : m_sentButtons)
            g_pSeatManager->sendPointerButton(t, b, WL_POINTER_BUTTON_STATE_RELEASED);
        g_pSeatManager->sendPointerFrame();
        m_sentButtons.clear();
    }
    m_drag = {};
    if (m_play.on)
        setPlay(false);
    m_typing = false;
    m_keys.fill(false);
    m_look    = {};
    m_lastAbs = {-1, -1};
    m_menu.hide();
    if (m_hold.key)
        place();
    m_aimed = -1;
    m_aimSurface.reset();
    m_pointerAt     = {};
    m_cursorShown   = false;
    m_lastSentLocal = {-1, -1};
    // (a game's pointer lock would hold the cursor where the game is, as Hyprland's keybinds let go of it too)
    g_pInputManager->unconstrainMouse();
    // Hyprland's cursor as ever, off the 3D monitor (it's never on it, away)
    lockCursors(false);
    g_pHyprRenderer->ensureCursorRenderingMode();
    if (refocus)
        g_pInputManager->simulateMouseMovement(); // what's under the cursor gets the pointer (and the keyboard, as input:follow_mouse says)
    const auto at = g_pCompositor->getMonitorFromCursor();
    log(std::format("the mouse and keyboard went to {}, 3D stays on {}", at ? at->m_name : "?", mon->m_name));
    if (!m_awayTold) {
        m_awayTold = true;
        notify(std::format("the mouse is on {} now; move it back onto {}, or press Super+Esc, to walk in 3D again", at ? at->m_name : "your desktop", mon->m_name));
    }
}

// the cursor came onto the 3D monitor: the mouse and keyboard are 3D's again (the cursor stays where it came in)
void CDesktop3D::comeBack() {
    const auto mon = m_monitor.lock();
    if (!m_away || !mon)
        return;
    m_away = false;
    m_keys.fill(false);
    m_look    = {};
    m_lastAbs = {-1, -1};
    lockCursors(true);
    g_pHyprRenderer->ensureCursorRenderingMode();
    Desktop::focusState()->rawMonitorFocus(mon);
    unfocusOthers(mon);
    log("the mouse and keyboard came back into 3D on " + mon->m_name);
}

// where Super+Esc puts the cursor: where it was last on another monitor, if that one is still there, else the middle
// of the monitor nearest to the 3D one; none when there's no other
std::optional<Vector2D> CDesktop3D::desktopSpot() const {
    const auto mon = m_monitor.lock();
    if (!mon)
        return std::nullopt;
    PHLMONITOR nearest;
    for (const auto& m : g_pCompositor->m_monitors) {
        if (!m || m == mon || m->m_size.x < 1 || m->m_size.y < 1)
            continue;
        if (m_desktopAt && CBox{m->m_position, m->m_size}.containsPoint(*m_desktopAt))
            return m_desktopAt;
        if (!nearest || m->middle().distanceSq(mon->middle()) < nearest->middle().distanceSq(mon->middle()))
            nearest = m;
    }
    if (!nearest)
        return std::nullopt;
    return nearest->middle();
}

// Super+Esc, hyprctl hypr3d away, hypr3d:away: the mouse and keyboard to the desktop on another monitor, the 3D view
// staying up; or back into 3D, the cursor onto the 3D monitor
std::string CDesktop3D::setAway(bool away) {
    const auto mon = m_monitor.lock();
    if (m_mode != MODE_ACTIVE || !mon)
        return "error: not in 3D";
    if (!away) {
        if (m_away) {
            if (g_pCompositor->getMonitorFromCursor() != mon)
                g_pCompositor->warpCursorTo(mon->middle(), true); // (the focus too)
            comeBack();
        }
        return "in 3D";
    }
    if (m_away)
        return "away";
    const auto to = desktopSpot();
    if (!to)
        return "error: there's no other monitor for the mouse to go to";
    g_pCompositor->warpCursorTo(*to, true); // (the focus too)
    goAway(true);
    return "away";
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
    endPlay();
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
    endPlay();
    m_play.t = 0;
    m_drawnSurfaces.clear();
    m_followLater.reset();
    lipSync(); // out of 3D: the microphone closes (update() no longer runs)
    if (!m_restoreLater)
        m_restoreLater = g_pEventLoopManager->doLaterLock([this] {
            m_restoreLater.reset(); // safe: the queue already moved this callback out
            restore();
        });
    if (const auto mon = m_monitor.lock())
        g_pHyprRenderer->damageMonitor(mon);
}

// aquamarine before 0.12.1 queues a headless output's late frame as an idle event that points at the output itself
// (CHeadlessOutput::framecb captures `this`; fixed upstream by 1699271 and 6ecde03), so when the output goes before the
// event runs, it runs on freed memory: Hyprland crashes in CBackend::dispatchIdle, or its heap is corrupted and malloc
// aborts later. In 3D the plugin asks for each frame as soon as the last one is out, so when its monitor is a headless
// one being removed, a slow frame is nearly always queued so (and one can be just after 3D, or on any headless monitor
// that draws). An idle event of ours, queued after that one, holds a removed headless output until then
void CDesktop3D::holdOutput(const PHLMONITOR& mon) {
    const auto backend = g_pCompositor->m_aqBackend;
    if (!mon || !mon->m_output || !backend)
        return;
    if (const auto impl = mon->m_output->getBackend(); !impl || impl->type() != Aquamarine::AQ_BACKEND_HEADLESS)
        return;
    // (the ones whose event has run are held only here)
    std::erase_if(m_outputHolds, [](const auto& h) { return h.strongRef() <= 1; });
    auto hold = makeShared<std::function<void()>>([out = mon->m_output]() mutable { out.reset(); });
    m_outputHolds.emplace_back(hold);
    backend->addIdleEvent(hold);
    log(mon->m_name + " removed: its output held until aquamarine's idle events queued before it have run");
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
    m_away   = false;
    endPlay();
    m_play.t = 0;
    m_keys.fill(false);
    if (m_hold.key)
        place();
    // placed windows were kept awake on hidden workspaces, let Hyprland suspend them again
    if (!m_placements.empty())
        g_pCompositor->updateSuspendedStates();
    m_aimed = -1;
    m_aimSurface.reset();
    m_lastSentLocal = {-1, -1};
    m_pointerAt     = {};
    m_panels.clear();
    m_drawnSurfaces.clear();

    g_pHyprRenderer->m_directScanoutBlocked = m_prevDSBlocked;
    lockCursors(false);
    g_pHyprRenderer->setCursorHidden(false);
    g_pHyprRenderer->ensureCursorRenderingMode();
    g_pInputManager->simulateMouseMovement();

    if (const auto mon = m_monitor.lock())
        g_pHyprRenderer->damageMonitor(mon);

    log("left 3D");
}

void CDesktop3D::setTyping(bool on) {
    if (m_mode != MODE_ACTIVE || m_away)
        return;
    if (!on && m_play.on) {
        setPlay(false); // Super+Esc ends play mode too
        return;
    }
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
    V3            fwd, up{0, 1, 0};
    if (m_mode == MODE_ACTIVE) {
        m_camera = view;
        fwd      = forwardFrom(m_camera.yaw, m_camera.pitch);
        // play mode: the camera goes to face the window played, turned as it is
        if (m_play.t > 0.f && m_play.framed) {
            const float k = smoothstep01(m_play.t);
            const V3    right{std::cos(m_camera.yaw), 0, std::sin(m_camera.yaw)};
            const Quat  q  = slerp(Quat::fromBasis(right, cross(right, fwd), -fwd), m_play.rot, k);
            m_camera.eye   = lerp(m_camera.eye, m_play.eye, k);
            fwd            = q.rotate({0, 0, -1});
            up             = q.rotate({0, 1, 0});
            m_camera.yaw   = std::atan2(fwd.x, -fwd.z);
            m_camera.pitch = std::asin(std::clamp(fwd.y, -1.f, 1.f));
        }
    } else {
        const SCamera a = flatCamera();
        const SCamera b = m_mode == MODE_ENTERING ? view : m_exitFrom;
        m_camera.eye    = lerp(a.eye, b.eye, e);
        m_camera.yaw    = a.yaw + wrapAngle(b.yaw - a.yaw) * e; // the short way round
        m_camera.pitch  = lerpf(a.pitch, b.pitch, e);
        fwd             = forwardFrom(m_camera.yaw, m_camera.pitch);
    }

    adaptExposure(dt);
    lipSync();
    animateAvatar(dt);
    // the apps' icons the menu's Apps and Windows pages show, a few a frame (finding one and drawing it takes a moment)
    if (m_menu.open() && (m_menu.path().find("apps") != std::string::npos || m_menu.path().find("windows") != std::string::npos)) {
        if (m_iconNext > m_apps.size())
            m_iconNext = 0;
        for (int n = 0; n < 3 && m_iconNext < m_apps.size(); ++n)
            appIcon(m_apps[m_iconNext++].icon, ICON_PX);
    } else
        m_iconNext = 0;
    m_menu.update(dt, (int)std::round(mon->m_transformedSize.x), (int)std::round(mon->m_transformedSize.y), (float)mon->m_scale);

    m_view          = M4::lookAt(m_camera.eye, m_camera.eye + fwd, up);
    m_camFwd        = fwd;
    m_camUp         = normalize(up - fwd * dot(up, fwd));
    const float far = m_world.model ? std::max(200.f, length(m_world.bounds.size()) * 1.5f) : 200.f;
    m_proj          = M4::perspective(FOV_Y, (float)(mon->m_size.x / mon->m_size.y), 0.05f, far);

    std::unordered_set<uintptr_t> placed;
    for (const auto& [key, pl] : m_placements)
        placed.insert(key);
    m_panels = collectPanels(mon, layerSpacing(), placed);
    updatePlacements(dt);
    layoutPanels(e);
    if (m_play.on || m_play.t > 0.f)
        updatePlay(dt);

    m_drawnSurfaces.clear();
    for (const auto& p : m_panels)
        for (const auto& s : p.surfaces)
            if (const auto res = s.surface.lock())
                m_drawnSurfaces.insert(res.get());

    if (m_mode == MODE_ACTIVE && !m_away) {
        if (m_play.on)
            aimPlay();
        else
            aim();
        updatePointer();
        addAppCursor();
    } else {
        m_aimed       = -1;
        m_pointerAt   = {};
        m_cursorShown = false;
    }

    // the mouse went to another monitor or came back without a move Hyprland tells of (a keybind's focus warping the
    // cursor to a monitor with no window on it): followed after this frame
    if (m_mode == MODE_ACTIVE && !m_followLater && cursorAway() != m_away)
        m_followLater = g_pEventLoopManager->doLaterLock([this] {
            m_followLater.reset(); // safe: the queue already moved this callback out
            followCursor();
        });
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

    // windows pinned to the view: down its right side, in the order they were pinned, pin_size of its height each,
    // 0.8 m in front of the eye (drawn over the world)
    const float aspect  = mon && mon->m_size.y > 0 ? (float)(mon->m_size.x / mon->m_size.y) : 16.f / 9.f;
    const float pinD    = 0.8f, pinHalfH = pinD * std::tan(FOV_Y * 0.5f), pinHalfW = pinHalfH * aspect, pinMargin = 0.05f * pinHalfH;
    const float pinSize = g_cfgPinSize ? g_cfgPinSize->value() : 0.3f;
    const auto  pinScale = [&](const SPanel& p) { return std::min(2.f * pinHalfH * pinSize / (float)p.box.h, (pinHalfW - 2.f * pinMargin) / (float)p.box.w); };
    std::vector<std::pair<uint64_t, uintptr_t>> pins;
    for (const auto& [key, pl] : m_placements)
        if (pl.pinned)
            pins.emplace_back(pl.pinned, key);
    std::ranges::sort(pins);
    std::unordered_map<uintptr_t, float> pinAbove;
    float                                pinAcc = 0;
    for (const auto& [order, key] : pins) {
        pinAbove[key] = pinAcc;
        for (const auto& p : m_panels)
            if (p.kind == PANEL_WINDOW && p.key == key)
                pinAcc += (float)p.box.h * pinScale(p) + pinMargin;
    }

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
        } else if (pl.pinned) {
            // with the view at once, turned with it
            const float scale = pinScale(p), w = (float)p.box.w * scale, h = (float)p.box.h * scale;
            const V3    right = normalize(cross(m_camFwd, m_camUp));
            pl.targetCenter   = eye + m_camFwd * pinD + right * (pinHalfW - pinMargin - w * 0.5f) + m_camUp * (pinHalfH - pinMargin - pinAbove[p.key] - h * 0.5f);
            pl.targetRot      = Quat::fromBasis(right, m_camUp, -m_camFwd);
            pl.targetScale    = scale;
            pl.center         = pl.targetCenter;
            pl.rot            = pl.targetRot;
            pl.scale          = pl.targetScale;
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
        p.front      = pl.pinned != 0 && inWorld; // (pinned to the view: over the world, as the HUD is)
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

    // pick it up from wherever it is drawn right now (pinned: not any more)
    SPlacement& pl = m_placements[key];
    pl.window      = w;
    pl.pinned      = 0;
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
    // it keeps sliding into the spot it was last aimed at, which its class remembers
    const uintptr_t key = m_hold.key;
    m_hold              = {};
    rememberSpot(key);
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
        // where it takes no input, through it to what's behind, as on the 2D desktop (a shell's overlay over the whole
        // screen, which takes input only at its edges)
        if (p.hitRoot && !p.hitRoot->at(local * p.hitScale - p.hitOffset, true).first)
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

    // (hidden by the world, unless it's drawn over it: pinned to the view)
    if (SRayHit hit; !m_panels[m_aimed].front && m_world.collision.raycast(eye, dir, best - 1e-3f, hit)) {
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

void CDesktop3D::updatePointer(uint32_t timeMs, bool frame) {
    SP<CWLSurfaceResource> surf;
    Vector2D               local;
    m_pointerAt = {};

    if (m_drag.surface && !(PROTO::data && PROTO::data->dndActive())) {
        // implicit grab: keep sending to the pressed surface, relative to its panel plane (in play mode: where the
        // pointer is over the window played). Not while something's dragged out of it: that goes where you point
        surf = m_drag.surface.lock();
        local = m_lastSentLocal;
        if (surf) {
            const auto played = m_play.on ? m_play.window.lock() : nullptr;
            const int  own    = played ? windowPanel(played) : -1;
            for (const auto& p : m_panels) {
                if (p.key != m_drag.panel)
                    continue;
                float    t = 0;
                Vector2D panelLocal;
                bool     on = false;
                if (m_play.on) {
                    on         = own >= 0;
                    panelLocal = on ? m_play.pointer - (p.box.pos() - m_panels[own].box.pos()) : Vector2D{};
                } else
                    on = p.pose.intersect(m_camera.eye, forwardFrom(m_camera.yaw, m_camera.pitch), t, panelLocal, false);
                if (on) {
                    local       = panelLocal * p.hitScale - p.hitOffset - m_drag.offset;
                    m_pointerAt = {p.key, panelLocal};
                }
                break;
            }
        } else
            m_drag = {};
    } else if (m_aimed >= 0) {
        surf        = m_aimSurface.lock();
        local       = m_aimLocal;
        m_pointerAt = {m_panels[m_aimed].key, m_aimPanelLocal};
    }

    // a drag (wl_data_device) goes where the crosshair is: over the surface it points at (Hyprland takes that as the
    // drag's, and gives no pointer focus while it lasts), moved there as Hyprland moves one, by its pointer move event
    // (which 3D keeps back otherwise)
    if (PROTO::data && PROTO::data->dndActive()) {
        g_pSeatManager->setPointerFocus(surf, local);
        if (surf && (surf != m_dndAt.lock() || local != m_dndLocal)) {
            m_dndAt    = surf;
            m_dndLocal = local;
            const auto hl = Desktop::View::CWLSurface::fromResource(surf);
            if (const auto box = hl ? hl->getSurfaceBoxGlobal() : std::nullopt) {
                Event::SCallbackInfo info;
                m_ownMove = true; // (a window on another monitor's workspace: the mouse doesn't go away there)
                Event::bus()->m_events.input.mouse.move.emit(box->pos() + local, info);
                m_ownMove = false;
            }
        }
        return;
    }

    const auto current = g_pSeatManager->m_state.pointerFocus.lock();
    if (surf != current) {
        g_pSeatManager->setPointerFocus(surf, local);
        m_lastSentLocal = local;
        if (surf && frame)
            g_pSeatManager->sendPointerFrame();
        return;
    }

    if (surf && local != m_lastSentLocal) {
        g_pSeatManager->sendPointerMotion(timeMs ? timeMs : nowMs(), local);
        if (frame)
            g_pSeatManager->sendPointerFrame();
        m_lastSentLocal = local;
    }
}

int CDesktop3D::windowPanel(const PHLWINDOW& w) const {
    const uintptr_t key = reinterpret_cast<uintptr_t>(w.get());
    for (size_t i = 0; i < m_panels.size(); ++i)
        if (m_panels[i].kind == PANEL_WINDOW && m_panels[i].key == key)
            return (int)i;
    return -1;
}

// ------------------------------------------------------------------ play mode

std::string CDesktop3D::setPlay(bool on) {
    if (!on) {
        if (!m_play.on)
            return "walking";
        endPlay();
        m_typing = false;
        m_keys.fill(false);
        log("play mode off");
        return "walking";
    }
    if (m_play.on)
        return "playing";
    if (m_mode != MODE_ACTIVE)
        return "error: not in 3D";
    if (m_away)
        setAway(false); // (played from a keybind or a script with the mouse on another monitor: back into 3D)
    if (m_aimed < 0 || m_aimed >= (int)m_panels.size() || m_panels[m_aimed].kind == PANEL_LAYER)
        return "error: point the crosshair at a window to play it";
    const SPanel& p = m_panels[m_aimed];
    auto          w = p.window.lock();
    // a dialog's window is played, the dialog with it (as it's over it on the 2D desktop)
    for (auto up = w ? parentOf(w) : nullptr; up && windowPanel(up) >= 0; up = parentOf(up))
        w = up;
    const int own = w ? windowPanel(w) : -1;
    if (own < 0)
        return "error: point the crosshair at a window to play it";

    if (m_hold.key)
        place();
    m_menu.hide();
    m_play.on         = true;
    m_play.window     = w;
    m_play.framed     = false;
    m_play.unfocused  = 0;
    m_play.byFullscreen = false; // (onFullscreen says so after)
    m_play.pointer    = p.box.pos() - m_panels[own].box.pos() + m_aimPanelLocal; // where the crosshair was
    m_play.lastGlobal = g_pPointerManager->position();
    // its keys, every one of them, as when typing into it (Super+Esc ends it), and the keyboard focus that games
    // want (SDL reads a controller only with it; a pointer lock is only active with it)
    m_typing = true;
    m_keys.fill(false);
    if (w != Desktop::focusState()->window())
        Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK);
    clampPlayPointer();
    notify(std::format("playing {}: Super+Esc gives the mouse and keyboard back", w->m_title.empty() ? w->m_class : w->m_title));
    return "playing";
}

void CDesktop3D::endPlay() {
    m_play.on = false;
    m_play.window.reset();
}

// a panel that's played along with the window: its popups, its dialogs (a file chooser, the portal's or its own:
// in the world in front of it, where they are on the 2D desktop) and theirs
bool CDesktop3D::playedWith(const SPanel& q, const PHLWINDOW& w) const {
    auto o = q.window.lock();
    for (int hops = 0; o && hops < 8; ++hops, o = parentOf(o))
        if (o == w)
            return true;
    return false;
}

// Every frame: play mode ends when the window goes from the 3D view (closed, or its workspace hidden) or something
// else takes the keyboard. The camera eases to where the window fills most of the view, facing it and turned with
// it, and follows it; after play mode, back to the player
void CDesktop3D::updatePlay(float dt) {
    const auto w = m_play.window.lock();
    const int  i = w ? windowPanel(w) : -1;
    if (m_play.on) {
        // (a dialog of its own can take the keyboard, and an X11 menu for a while)
        auto focus = Desktop::focusState()->window();
        if (focus && focus->isX11OverrideRedirect())
            focus = x11Owner(focus);
        for (int hops = 0; focus && focus != w && hops < 8; ++hops)
            focus = parentOf(focus);
        const bool  ours = w && focus == w;
        m_play.unfocused = Desktop::focusState()->window() ? 0.f : m_play.unfocused + dt;
        const char* why  = i < 0 ? "the window left the 3D view" :
              ours                      ? (!g_pInputManager->m_exclusiveLSes.empty() ? "a layer surface took the keyboard" : nullptr) :
              m_play.unfocused == 0     ? "another window has the keyboard" :
              m_play.unfocused >= 0.5f  ? "no window has had the keyboard for half a second" :
                                          nullptr;
        if (why) {
            log(std::string("play mode ended: ") + why);
            setPlay(false);
        }
    }
    m_play.t = std::clamp(m_play.t + (m_play.on ? dt : -dt) / PLAY_TIME, 0.f, 1.f);
    if (i < 0)
        return;

    const SPanel& p      = m_panels[i];
    const auto    mon    = m_monitor.lock();
    const float   aspect = mon && mon->m_size.y > 0 ? (float)(mon->m_size.x / mon->m_size.y) : 16.f / 9.f;
    const float   tanY   = std::tan(FOV_Y * 0.5f);
    const float   dist   = std::max((float)p.box.h * p.pose.scale * 0.5f / tanY, (float)p.box.w * p.pose.scale * 0.5f / (tanY * aspect)) / PLAY_FILL;
    const V3      eye    = p.pose.at(p.box.size() * 0.5) + p.pose.normal * dist;
    const Quat    rot    = Quat::fromBasis(p.pose.right, -p.pose.down, p.pose.normal);
    const float   k      = m_play.framed ? 1.f - std::exp(-dt * 12.f) : 1.f;
    m_play.eye           = lerp(m_play.eye, eye, k);
    m_play.rot           = slerp(m_play.rot, rot, k);
    m_play.framed        = true;
    if (!m_play.on)
        return;

    // over everything else while it's played: nothing in the world gets in front of it
    for (auto& q : m_panels)
        if (playedWith(q, w))
            q.front = true;

    // a warp (wp_pointer_warp_v1, or one of Hyprland's) is the only thing that moves Hyprland's cursor in 3D: the
    // pointer goes there
    if (const Vector2D g = g_pPointerManager->position(); g != m_play.lastGlobal) {
        m_play.lastGlobal = g;
        const auto hl     = p.hitRoot ? Desktop::View::CWLSurface::fromResource(p.hitRoot) : nullptr;
        if (const auto box = hl ? hl->getSurfaceBoxGlobal() : std::nullopt) {
            m_play.pointer = (g - box->pos() + p.hitOffset) / p.hitScale;
            clampPlayPointer();
        }
    }
}

// the pointer in play mode: in the confinement the window asked for (zwp_confined_pointer_v1, as Hyprland keeps it
// there on the 2D desktop), else over the window and its popups, as if they were a monitor
void CDesktop3D::clampPlayPointer() {
    const auto w   = m_play.window.lock();
    const int  own = w ? windowPanel(w) : -1;
    if (own < 0)
        return;
    const SPanel& o = m_panels[own];
    if (g_pInputManager->isConstrained() && !g_pInputManager->isLocked()) {
        const auto hl = Desktop::View::CWLSurface::fromResource(Desktop::focusState()->surface());
        const auto c  = hl ? hl->constraint() : nullptr;
        if (const auto box = hl ? hl->getSurfaceBoxGlobal() : std::nullopt; c && box && hl->resource() == o.hitRoot) {
            const Vector2D global = c->logicConstraintRegion().closestPoint(box->pos() + m_play.pointer * o.hitScale - o.hitOffset);
            m_play.pointer        = (global - box->pos() + o.hitOffset) / o.hitScale;
            return;
        }
    }
    Vector2D lo{0, 0}, hi = o.box.size();
    for (const auto& p : m_panels) {
        if (p.kind == PANEL_LAYER || !playedWith(p, w))
            continue;
        const Vector2D at = p.box.pos() - o.box.pos();
        lo                = {std::min(lo.x, at.x), std::min(lo.y, at.y)};
        hi                = {std::max(hi.x, at.x + p.box.w), std::max(hi.y, at.y + p.box.h)};
    }
    m_play.pointer = {std::clamp(m_play.pointer.x, lo.x, hi.x - 0.01), std::clamp(m_play.pointer.y, lo.y, hi.y - 0.01)};
}

// what the pointer is over in play mode: a popup of the window played or of a dialog of it (the last drawn on top),
// else a dialog of it, else the window
void CDesktop3D::aimPlay() {
    m_aimed = -1;
    m_aimSurface.reset();
    const auto w   = m_play.window.lock();
    const int  own = w ? windowPanel(w) : -1;
    if (own < 0)
        return;
    int      best = own;
    Vector2D local = m_play.pointer;
    for (int pass = 0; pass < 2 && best == own; ++pass) {
        for (int i = (int)m_panels.size() - 1; i >= 0; --i) {
            const SPanel& p = m_panels[i];
            if (i == own || p.kind != (pass == 0 ? PANEL_POPUP : PANEL_WINDOW) || p.alpha < 0.05f || !playedWith(p, w))
                continue;
            const Vector2D l = m_play.pointer - (p.box.pos() - m_panels[own].box.pos());
            if (l.x >= 0 && l.y >= 0 && l.x <= p.box.w && l.y <= p.box.h) {
                best  = i;
                local = l;
                break;
            }
        }
    }
    m_aimed         = best;
    m_aimPanelLocal = local;
    const SPanel& p = m_panels[best];
    if (p.hitRoot) {
        const auto [surf, sl] = p.hitRoot->at(local * p.hitScale - p.hitOffset, true);
        if (surf) {
            m_aimSurface = surf;
            m_aimLocal   = sl;
        }
    }
}

// the mouse in play mode, as Hyprland's CInputManager::onMouseMoved gives it on the 2D desktop: relative motion
// always (what a game uses under a pointer lock), and the pointer moves unless it's locked
void CDesktop3D::playMotion(const IPointer::SMotionEvent& e) {
    static auto PNOACCEL = CConfigValue<Config::INTEGER>("input:force_no_accel");
    Vector2D    delta = e.delta, unaccel = e.unaccel;
    if (e.device && e.device->m_isTouchpad) {
        if (e.device->m_flipX) {
            delta.x   = -delta.x;
            unaccel.x = -unaccel.x;
        }
        if (e.device->m_flipY) {
            delta.y   = -delta.y;
            unaccel.y = -unaccel.y;
        }
    }
    PROTO::relativePointer->sendRelativeMotion((uint64_t)e.timeMs * 1000, delta, unaccel);
    if (!g_pInputManager->isLocked()) {
        m_play.pointer += *PNOACCEL == 1 ? unaccel : delta;
        clampPlayPointer();
        aimPlay();
        updatePointer(e.timeMs, false);
    }
    g_pSeatManager->sendPointerFrame();
}

std::string CDesktop3D::playStatus() const {
    const auto w = m_play.window.lock();
    if (!m_play.on || !w)
        return "null";
    const bool locked = g_pInputManager->isLocked();
    return std::format(R"({{"class": "{}", "title": "{}", "pointer": [{:.1f}, {:.1f}], "locked": {}, "confined": {}, "view": {:.2f}}})", jsonEscape(w->m_class),
                       jsonEscape(w->m_title), m_play.pointer.x, m_play.pointer.y, locked, !locked && g_pInputManager->isConstrained(), m_play.t);
}

// the app's own cursor where the pointer is on a panel: the shape it asked for (wp_cursor_shape_v1, from Hyprland's
// cursor theme) or its own cursor surface, as Hyprland's pointer manager has it; nothing while it hides it (a game)
void CDesktop3D::addAppCursor() {
    static auto PINVISIBLE = CConfigValue<Config::INTEGER>("cursor:invisible");
    m_cursorShown          = false;
    if (!m_pointerAt.panel || !m_hookSoftCursor || *PINVISIBLE || m_menu.visible() || (!g_pSeatManager->m_state.pointerFocus && !(PROTO::data && PROTO::data->dndActive())))
        return;
    const auto tex  = g_pPointerManager->getCurrentCursorTexture();
    const auto size = g_pPointerManager->cursorSizeLogical();
    if (!tex || size.x < 1 || size.y < 1)
        return;
    for (auto& p : m_panels) {
        if (p.key != m_pointerAt.panel)
            continue;
        SPanelSurface c;
        c.tex = tex;
        c.box = CBox{m_pointerAt.local - g_pPointerManager->hotspot(), size};
        p.surfaces.emplace_back(std::move(c));
        m_cursorShown = true;
        return;
    }
}

// ------------------------------------------------------------------ input

bool CDesktop3D::onRelativeMotion(const IPointer::SMotionEvent& e) {
    if (m_mode == MODE_OFF || m_away)
        return false;
    if (m_mode == MODE_ACTIVE && m_play.on) {
        playMotion(e);
        return true;
    }
    const Vector2D delta = e.unaccel != Vector2D{} ? e.unaccel : e.delta;
    if (m_mode == MODE_ACTIVE && m_menu.open())
        m_menu.move((float)delta.x, (float)delta.y);
    else if (m_mode == MODE_ACTIVE)
        m_look += delta;
    return true;
}

bool CDesktop3D::onAbsoluteMotion(const Vector2D& abs) {
    if (m_mode == MODE_OFF || m_away)
        return false;
    // play mode: a tablet (or a nested session's pointer) covers the window played, as it would a monitor
    if (m_mode == MODE_ACTIVE && m_play.on) {
        const auto w   = m_play.window.lock();
        const int  own = w ? windowPanel(w) : -1;
        if (own >= 0) {
            m_play.pointer = abs * m_panels[own].box.size();
            clampPlayPointer();
            aimPlay();
            updatePointer();
        }
        m_lastAbs = abs;
        return true;
    }
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

    // away on another monitor the keys are the desktop's there; Super+Esc comes back into 3D
    if (m_away) {
        if (k == K_ESC && meta && m_mode == MODE_ACTIVE) {
            setAway(false);
            info.cancelled = true;
            m_consumed.insert(k);
        }
        return;
    }

    if (m_typing) {
        if (k == K_ESC && meta) {
            setTyping(false);
            info.cancelled = true;
            m_consumed.insert(k);
        }
        return;
    }

    // Super+Esc, walking: the mouse and keyboard to the desktop on another monitor, the 3D view staying up (with no
    // other monitor, it's Hyprland's)
    if (k == K_ESC && meta && m_mode == MODE_ACTIVE && desktopSpot()) {
        info.cancelled = true;
        m_consumed.insert(k);
        setAway(true);
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
        case K_X: // (and its class opens on the wall again)
            if (m_hold.key) {
                const uintptr_t key = m_hold.key;
                m_hold              = {};
                forgetSpot(key);
                returnToWall(key);
            } else if (m_aimed >= 0 && m_panels[m_aimed].kind != PANEL_LAYER) {
                const auto w = m_panels[m_aimed].window.lock();
                if (w) {
                    forgetSpot(reinterpret_cast<uintptr_t>(w.get()));
                    returnToWall(reinterpret_cast<uintptr_t>(w.get()));
                }
            }
            return;
        case K_E:
        case K_ENTER: setTyping(true); return;
        case K_P:
            if (const std::string r = setPlay(true); r.starts_with("error: "))
                notify(r.substr(7), true);
            return;
        case K_Q: m_menu.show("apps"); return;    // (WaylandCraft's launcher is V, the view here)
        case K_B: m_menu.show("windows"); return; // (WaylandCraft's window manager is B too)
        case K_H: // pins the window under the crosshair (or the one carried) to your view; H again puts it down
            if (const std::string r = togglePin(); r.starts_with("error: "))
                notify(r.substr(7), true);
            return;
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

    if (m_mode == MODE_OFF || m_away)
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
    if (m_mode == MODE_OFF || m_away)
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
        const float    notches = e.deltaDiscrete != 0 ? e.deltaDiscrete / 120.f : (float)e.delta / 15.f;
        const uint32_t mods    = g_pInputManager->getModsFromAllKBs();
        if (mods & HL_MODIFIER_CTRL)
            m_hold.scaleMul = std::clamp(m_hold.scaleMul * std::pow(0.92f, notches), 0.2f, 5.f);
        else if (mods & HL_MODIFIER_SHIFT) { // its real size: the app draws itself anew (text as big as it was)
            if (const auto it = m_placements.find(m_hold.key); it != m_placements.end())
                if (const auto w = it->second.window.lock())
                    resizeReal(w, w->m_realSize->goal() * std::pow(0.9, notches));
        } else
            m_hold.dist = std::clamp(m_hold.dist * std::pow(0.9f, notches), 0.6f, 12.f);
        return;
    }

    // third person, not pointing at anything that scrolls: the wheel zooms
    if (m_mode == MODE_ACTIVE && m_thirdPerson && !m_play.on && m_aimSurface.expired() && e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        const float notches = e.deltaDiscrete != 0 ? e.deltaDiscrete / 120.f : (float)e.delta / 15.f;
        m_camDist           = std::clamp(m_camDist * std::pow(1.12f, notches), 0.8f, 10.f);
        return;
    }

    if (m_mode != MODE_ACTIVE || m_aimSurface.expired())
        return;

    // to the window as Hyprland sends it on the 2D desktop (CInputManager::onMouseWheel): the scroll factor (a window
    // rule's first, then the device's own, then input's or input:touchpad's), and for clients without high-resolution
    // scrolling whole notches made up from a high-resolution wheel's (input:emulate_discrete_scroll: the first at
    // once, then one per notch's worth, afresh after half a second or a change of direction)
    static auto PSCROLL   = CConfigValue<Config::FLOAT>("input:scroll_factor");
    static auto PTPSCROLL = CConfigValue<Config::FLOAT>("input:touchpad:scroll_factor");
    static auto PEMULATE  = CConfigValue<Config::INTEGER>("input:emulate_discrete_scroll");
    const bool  touchpad  = *PTPSCROLL <= 0.f || e.source == WL_POINTER_AXIS_SOURCE_FINGER;
    double      factor    = touchpad ? *PTPSCROLL : *PSCROLL;
    if (const auto dev = g_wheelPointer.lock(); dev && dev->m_scrollFactor.has_value())
        factor = *dev->m_scrollFactor;
    if (m_aimed >= 0 && m_aimed < (int)m_panels.size()) {
        if (const auto w = m_panels[m_aimed].window.lock()) {
            if (!touchpad && w->isScrollMouseOverridden())
                factor = w->getScrollMouse();
            else if (touchpad && w->isScrollTouchpadOverridden())
                factor = w->getScrollTouchpad();
        }
    }

    double discrete = e.deltaDiscrete != 0 ? factor * e.deltaDiscrete / std::abs(e.deltaDiscrete) : 0;
    double delta    = e.delta * factor;
    if (e.source == WL_POINTER_AXIS_SOURCE_WHEEL && ((*PEMULATE >= 1 && std::abs(e.deltaDiscrete) != 120) || *PEMULATE >= 2)) {
        auto&     wh       = m_wheel;
        const int interval = factor != 0 ? (int)std::round(120 * (1 / factor)) : 120;
        if (std::signbit((double)e.deltaDiscrete) != wh.negative || e.axis != wh.axis || e.timeMs - wh.timeMs > 500) {
            wh.acc   = 0;
            discrete = std::copysign(1, e.deltaDiscrete);
        } else
            discrete = 0;
        for (; (int)wh.acc >= interval; wh.acc -= interval)
            discrete += std::copysign(1, e.deltaDiscrete);
        wh.negative = std::signbit((double)e.deltaDiscrete);
        wh.axis     = e.axis;
        wh.timeMs   = e.timeMs;
        wh.acc += std::abs(e.deltaDiscrete);
        delta = 15.0 * discrete * factor;
    }
    const int32_t value120 = std::round(factor * e.deltaDiscrete);
    const int32_t steps    = std::abs(discrete) != 0 && std::abs(discrete) < 1 ? std::copysign(1, discrete) : std::round(discrete);

    g_pSeatManager->sendPointerAxis(e.timeMs, e.axis, delta, steps, value120, e.source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    // a touchpad's frame waits for the device's, as in Hyprland: both axes of a two-finger scroll go in one
    m_axisFramePending = (e.source == WL_POINTER_AXIS_SOURCE_FINGER || e.source == WL_POINTER_AXIS_SOURCE_CONTINUOUS) && m_hookFrame;
    if (!m_axisFramePending)
        g_pSeatManager->sendPointerFrame();
}

void CDesktop3D::onPointerFrame() {
    if (!m_axisFramePending)
        return;
    m_axisFramePending = false;
    g_pSeatManager->sendPointerFrame();
}

// --------------------------------------------------------- apps and windows

namespace {
    std::string lowered(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    std::string classOf(const PHLWINDOW& w) {
        return w->m_class.empty() ? w->m_initialClass : w->m_class;
    }

    // the window a dialog belongs to. (For X11 windows Hyprland 0.55.2's CWindow::x11TransientFor() walks
    // WM_TRANSIENT_FOR past the top and then finds a window with no X11 surface: a Wayland one)
    PHLWINDOW parentOf(const PHLWINDOW& w) {
        if (!w->m_isX11)
            return w->parent();
        const auto xs = w->m_xwaylandSurface.lock();
        auto       up = xs ? xs->m_parent.lock() : nullptr;
        for (int hops = 0; up && up->m_parent && hops < 16; ++hops)
            up = up->m_parent.lock();
        if (up)
            for (const auto& o : g_pCompositor->m_windows)
                if (o && o != w && o->m_isMapped && o->m_xwaylandSurface.lock() == up)
                    return o;
        return nullptr;
    }

    // a window of its own (not a dialog, a menu or a tooltip)
    bool toplevel(const PHLWINDOW& w) {
        return w && w->m_isMapped && !w->isHidden() && !w->isX11OverrideRedirect() && !parentOf(w);
    }

    // a process and its parents up to init, from /proc/PID/stat: anyone can read that, where a process's environment
    // can be closed (Chromium's, Electron's)
    std::vector<int64_t> ancestry(int64_t pid) {
        std::vector<int64_t> out;
        while (pid > 1 && out.size() < 64) {
            out.push_back(pid);
            std::ifstream in(std::format("/proc/{}/stat", pid));
            std::string   s;
            const size_t  name = std::getline(in, s) ? s.rfind(')') : std::string::npos; // (past its name: anything)
            if (name == std::string::npos)
                break;
            std::istringstream rest(s.substr(name + 1));
            std::string        state;
            if (!(rest >> state >> pid))
                break;
        }
        return out;
    }

    std::vector<std::string> commaList(const SP<Config::Values::CStringValue>& v) {
        std::vector<std::string> out;
        std::string              s = v ? v->value() : "";
        if (s == "[[EMPTY]]")
            return out;
        std::istringstream in(s);
        for (std::string part; std::getline(in, part, ',');) {
            const size_t a = part.find_first_not_of(" \t"), b = part.find_last_not_of(" \t");
            if (a != std::string::npos)
                out.push_back(part.substr(a, b - a + 1));
        }
        return out;
    }
}

const std::vector<SAppEntry>& CDesktop3D::apps() {
    const auto now = std::chrono::steady_clock::now();
    if (m_appsRead == std::chrono::steady_clock::time_point{} || now - m_appsRead > std::chrono::seconds(30)) {
        m_apps     = readDesktopEntries();
        m_appsRead = now;
    }
    return m_apps;
}

// Starts an app, a desktop entry's (by its id or name) or a command, as Hyprland's exec does, with a token in its
// environment: the window that has it goes in front of you. One that opens nothing of its own (a running app showing
// a window of its first instance's) is known by its class instead, for a minute
std::string CDesktop3D::launch(const std::string& what) {
    std::string cmd = what, cls, name = what;
    if (const SAppEntry* e = findApp(apps(), what)) {
        cmd  = e->terminal ? inTerminal(e->exec) : e->exec;
        cls  = e->wmClass.empty() ? e->id : e->wmClass;
        name = e->name;
    } else {
        // a command: the class its window likely has, for a window whose process can't be read (Chromium's) or isn't
        // this one's. A desktop entry that runs the same program says, else the program's own name
        std::string        prog;
        std::istringstream in(what);
        for (std::string word; in >> word;) {
            if (word.find('=') != std::string::npos && word.find('/') == std::string::npos)
                continue; // (VAR=value)
            prog = std::filesystem::path(word).filename().string();
            break;
        }
        for (const auto& e : apps()) {
            std::istringstream ex(e.exec);
            std::string        first;
            if (ex >> first; !prog.empty() && std::filesystem::path(first).filename() == prog) {
                cls = e.wmClass.empty() ? e.id : e.wmClass;
                break;
            }
        }
        if (cls.empty())
            cls = prog;
    }
    if (cmd.empty())
        return "error: nothing to launch";
    // Steam starts a game from its own running process, not as this one's child (or, starting itself, opens windows of
    // its own first): the game's window is the one with SteamAppId=ID in its environment, as Steam starts every game,
    // or, when that can't be read, class steam_app_ID, as Proton's are
    SLaunch l;
    if (const size_t at = what.find("steam://rungameid/"); at != std::string::npos) {
        l.steam = what.substr(at + 18, what.find_first_not_of("0123456789", at + 18) - (at + 18));
        if (!l.steam.empty())
            cls = "steam_app_" + l.steam;
    }
    l.token = std::format("{}-{}", getpid(), ++m_launchCount);
    l.what  = name;
    l.cls   = cls;
    l.at    = std::chrono::steady_clock::now();
    const auto pid = Config::Supplementary::executor()->spawnRawProc(std::format("export HYPR3D_LAUNCH={}; {}", l.token, cmd));
    if (!pid || !*pid)
        return "error: couldn't start " + name;
    l.pid = (int64_t)*pid;
    std::erase_if(m_launches, [&](const SLaunch& o) { return l.at - o.at > std::chrono::seconds(60); });
    m_launches.push_back(l);
    notify(std::format("starting {}{}", name, m_mode == MODE_ACTIVE ? ": it opens in front of you" : ""));
    return std::format("launched {} (pid {})", name, *pid);
}

// A window that opens: launched from 3D (its process is one we started or a child of it, its environment has our
// token, or, neither known, its class is the one a launch waits for) it goes where its class was put before, else in
// front of you as the app rules have it; any other that opens in 3D goes where its class was put before, when it's
// the only one of its class, else in front of you too. Dialogs stay by their parents
void CDesktop3D::onWindowOpen(const PHLWINDOW& w) {
    if (!w || w->isX11OverrideRedirect())
        return;
    // (a window closed while out of 3D can leave its placement behind, and a new one can come at its address)
    std::erase_if(m_placements, [](const auto& kv) { return kv.second.window.expired(); });
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_launches, [&](const SLaunch& l) { return now - l.at > std::chrono::seconds(60); });
    const std::string cls = classOf(w);
    bool              launched = false;
    const auto        env      = w->getEnv();
    const auto        token    = env.find("HYPR3D_LAUNCH");
    const auto        steamId  = env.contains("SteamAppId") ? env.at("SteamAppId") : env.contains("SteamGameId") ? env.at("SteamGameId") : "";
    const auto        parents  = m_launches.empty() ? std::vector<int64_t>{} : ancestry(w->getPID());
    for (auto it = m_launches.begin(); it != m_launches.end(); ++it) {
        bool ours = false, surely = false; // (surely: by its process, so every window it opens comes here)
        if (!it->steam.empty())
            ours = steamId == it->steam || (steamId.empty() && lowered(cls) == lowered(it->cls));
        else if ((token != env.end() && token->second == it->token) || std::ranges::contains(parents, it->pid))
            ours = surely = true;
        else
            ours = token == env.end() && !it->cls.empty() && lowered(it->cls) == lowered(cls);
        if (ours) {
            launched = true;
            logf("{} opened a window ({})", it->what, cls);
            if (!surely || cls == it->cls)
                m_launches.erase(it);
            break;
        }
    }
    if ((m_mode != MODE_ACTIVE && m_mode != MODE_ENTERING) || m_placements.contains(reinterpret_cast<uintptr_t>(w.get())))
        return; // (in 2D Hyprland lays it out, as always)
    // a dialog of a window out in the world (settings, a file chooser through the portal): in front of it, a little
    // towards you, where it opens over its parent on the 2D desktop
    if (const auto parent = parentOf(w); parent && w->m_isMapped) {
        const auto pp = m_placements.find(reinterpret_cast<uintptr_t>(parent.get()));
        if (pp == m_placements.end() || pp->second.returning)
            return;
        if (pp->second.pinned) { // (it follows the view: not there, in front of you)
            placeInFront(w, {});
            return;
        }
        const SPlacement& o  = pp->second;
        const Vector2D    at = (w->m_realPosition->goal() + w->m_realSize->goal() * 0.5) - (parent->m_realPosition->goal() + parent->m_realSize->goal() * 0.5);
        SPlacement        pl;
        pl.window = w;
        pl.center = pl.targetCenter = o.targetCenter + o.targetRot.rotate({(float)at.x * o.targetScale, -(float)at.y * o.targetScale, 0.06f});
        pl.rot = pl.targetRot = o.targetRot;
        pl.scale = pl.targetScale = o.targetScale;
        m_placements[reinterpret_cast<uintptr_t>(w.get())] = pl;
        return;
    }
    if (!toplevel(w))
        return;
    loadSpots();
    const auto spot = m_spots.find(cls);
    const bool alone = std::ranges::count_if(g_pCompositor->m_windows, [&](const PHLWINDOW& o) { return toplevel(o) && classOf(o) == cls; }) == 1;
    if (spot != m_spots.end() && (launched || alone))
        placeAt(w, spot->second);
    else if (launched)
        placeInFront(w, appRule(m_rules, cls));
    else if (m_mode == MODE_ACTIVE && w->m_monitor.lock() == m_monitor.lock() && w->m_workspace && w->m_workspace->isVisible() && w->m_realSize->goal().y >= 1) {
        // any other of its own that opens while you're in 3D, on the 3D monitor (a terminal from a keybind, the
        // screen-share portal's picker, a splash): in front of you rather than out of sight on the wall; a floating
        // one as big as it is on the wall
        const float scale = m_screen.scale();
        placeInFront(w, w->m_isFloating ? SAppRule{.distance = 1.3f, .height = (float)w->m_realSize->goal().y * scale} : appRule(m_rules, cls));
    }
}

// a window going fullscreen in 3D (a video, a game) while it has the keyboard is played: it fills your view, and gets
// the mouse and keys; leaving fullscreen ends that. (Maximized is one of Hyprland's fullscreen modes too: that only
// makes the window bigger)
void CDesktop3D::onFullscreen(const PHLWINDOW& w) {
    if (m_mode != MODE_ACTIVE || !w)
        return;
    if (!w->isEffectiveInternalFSMode(FSMODE_FULLSCREEN)) {
        if (m_play.on && m_play.byFullscreen && m_play.window.lock() == w)
            setPlay(false);
        return;
    }
    if (m_play.on || w != Desktop::focusState()->window() || windowPanel(w) < 0)
        return;
    const auto& p = m_panels[windowPanel(w)];
    m_aimed         = windowPanel(w);
    m_aimPanelLocal = p.box.size() * 0.5;
    if (setPlay(true) == "playing")
        m_play.byFullscreen = true;
}

// out in the world in front of you, as the rule has it: its middle `distance` ahead of the eye (level with it), `side`
// to the right, `height` tall, turned to face you, and nearer if a wall is in the way
bool CDesktop3D::placeInFront(const PHLWINDOW& w, const SAppRule& rule) {
    const Vector2D size = w->m_realSize->goal();
    if (size.x < 1 || size.y < 1)
        return false;
    const V3 eye = playerCamera().eye;
    const V3 fwd{std::sin(m_yaw), 0, -std::cos(m_yaw)}, right{std::cos(m_yaw), 0, std::sin(m_yaw)};
    V3       c = eye + fwd * rule.distance + right * rule.side;
    const V3 d = c - eye;
    if (SRayHit hit; m_world.collision.raycast(eye, d / length(d), length(d), hit))
        c = eye + d / length(d) * std::max(0.4f, hit.t - 0.15f);
    const V3   n = normalize(V3{eye.x - c.x, 0, eye.z - c.z});
    const V3   r = normalize(cross(V3{0, 1, 0}, n));
    SPlacement pl;
    pl.window = w;
    pl.center = pl.targetCenter = c;
    pl.rot = pl.targetRot = Quat::fromBasis(r, cross(n, r), n);
    pl.scale = pl.targetScale = rule.height / (float)size.y;
    m_placements[reinterpret_cast<uintptr_t>(w.get())] = pl;
    return true;
}

void CDesktop3D::placeAt(const PHLWINDOW& w, const SWindowSpot& spot) {
    SPlacement pl;
    pl.window = w;
    pl.center = pl.targetCenter = spot.center;
    pl.rot = pl.targetRot = spot.rot;
    pl.scale = pl.targetScale = spot.scale;
    m_placements[reinterpret_cast<uintptr_t>(w.get())] = pl;
}

void CDesktop3D::loadSpots() {
    if (m_spotsFor == m_mapPath)
        return;
    m_spots    = readWindowSpots(m_mapPath);
    m_spotsFor = m_mapPath;
}

// where a window was put, kept for its class (in this world): its windows open there again
void CDesktop3D::rememberSpot(uintptr_t key) {
    const auto it = m_placements.find(key);
    const auto w  = it != m_placements.end() ? it->second.window.lock() : nullptr;
    if (!w || it->second.returning || it->second.pinned || !toplevel(w))
        return;
    loadSpots();
    m_spots[classOf(w)] = {it->second.targetCenter, it->second.targetRot, it->second.targetScale};
    saveWindowSpots(m_mapPath, m_spots);
}

void CDesktop3D::forgetSpot(uintptr_t key) {
    const auto it = m_placements.find(key);
    const auto w  = it != m_placements.end() ? it->second.window.lock() : nullptr;
    if (!w)
        return;
    loadSpots();
    if (m_spots.erase(classOf(w)))
        saveWindowSpots(m_mapPath, m_spots);
}

// entering 3D: the windows whose class was put somewhere go there again (when there's one of the class)
void CDesktop3D::restoreSpots() {
    std::erase_if(m_placements, [](const auto& kv) { return kv.second.window.expired(); });
    loadSpots();
    if (m_spots.empty())
        return;
    std::unordered_map<std::string, int> count;
    for (const auto& w : g_pCompositor->m_windows)
        if (toplevel(w))
            ++count[classOf(w)];
    for (const auto& w : g_pCompositor->m_windows) {
        if (!toplevel(w) || !m_spots.contains(classOf(w)))
            continue;
        if (count[classOf(w)] != 1 || m_placements.contains(reinterpret_cast<uintptr_t>(w.get()))) {
            logf("{}: not to its place ({} of its class, {})", classOf(w), count[classOf(w)], m_placements.contains(reinterpret_cast<uintptr_t>(w.get())) ? "placed already" : "on the wall");
            continue;
        }
        placeAt(w, m_spots[classOf(w)]);
        logf("{}: to its place", classOf(w));
    }
}

PHLWINDOW CDesktop3D::findWindow(const std::string& what) const {
    if (what.starts_with("0x")) {
        const uintptr_t addr = std::strtoull(what.c_str() + 2, nullptr, 16);
        for (const auto& w : g_pCompositor->m_windows)
            if (w && reinterpret_cast<uintptr_t>(w.get()) == addr)
                return w;
        return nullptr;
    }
    const std::string lw = lowered(what);
    for (const bool byTitle : {false, true})
        for (const auto& w : g_pCompositor->m_windows)
            if (toplevel(w) && lowered(byTitle ? w->m_title : classOf(w)) == lw)
                return w;
    return nullptr;
}

std::string CDesktop3D::windowAction(const PHLWINDOW& w, eWindowAction a) {
    if (!w || !w->m_isMapped)
        return "error: no such window";
    const uintptr_t key    = reinterpret_cast<uintptr_t>(w.get());
    const auto      placed = m_placements.find(key);
    switch (a) {
        case WA_FOCUS: {
            const auto r = Config::Actions::focus(w);
            return r ? "focused" : "error: " + r.error().message;
        }
        case WA_BRING:
            if (m_mode != MODE_ACTIVE)
                return "error: not in 3D";
            if (placed != m_placements.end() && placed->second.pinned)
                placed->second.pinned = 0;
            return placeInFront(w, appRule(m_rules, classOf(w))) ? "here" : "error: it has no size yet";
        case WA_WALL:
            forgetSpot(key);
            returnToWall(key);
            return "on the wall";
        case WA_PIN: return setPinned(w, placed == m_placements.end() || !placed->second.pinned);
        case WA_BIGGER:
        case WA_SMALLER: return resizeReal(w, w->m_realSize->goal() * (a == WA_BIGGER ? 1.25 : 0.8));
        case WA_PLAY: {
            if (m_mode != MODE_ACTIVE)
                return "error: not in 3D";
            if (windowPanel(w) < 0 && placed == m_placements.end())
                placeInFront(w, appRule(m_rules, classOf(w))); // (from another workspace: out here first)
            const int i = windowPanel(w);
            if (i < 0) { // (it shows from the next frame)
                m_play.on = false;
                return "error: it isn't in the 3D view yet, try again";
            }
            m_aimed         = i;
            m_aimPanelLocal = m_panels[i].box.size() * 0.5;
            return setPlay(true);
        }
        case WA_CLOSE: {
            const auto r = Config::Actions::closeWindow(w);
            return r ? "closing" : "error: " + r.error().message;
        }
    }
    return "error: no such action";
}

// pinned: it follows the view, in its top right corner (the next one pinned under it); unpinned it stays where it is
std::string CDesktop3D::setPinned(const PHLWINDOW& w, bool on) {
    if (m_mode != MODE_ACTIVE)
        return "error: not in 3D";
    const uintptr_t key = reinterpret_cast<uintptr_t>(w.get());
    auto            it  = m_placements.find(key);
    if (!on) {
        if (it == m_placements.end() || !it->second.pinned)
            return "unpinned";
        auto& pl  = it->second;
        pl.pinned = 0;
        // where it is as you see it, but not in a wall you stand close to (it was drawn over the world): brought nearer
        // your eye, and smaller so it looks the same, till nothing is between you and its middle or its corners
        if (const int i = windowPanel(w); i >= 0) {
            const CBox&      box  = m_panels[i].box;
            const SPanelPose pose = poseFrom(pl.center, pl.rot, pl.scale, box);
            const V3         eye  = m_camera.eye;
            float            k    = 1.f;
            for (const Vector2D& at : {box.size() * 0.5, Vector2D{0.0, 0.0}, Vector2D{box.w, 0.0}, Vector2D{0.0, box.h}, box.size()}) {
                const V3    d   = pose.at(at) - eye;
                const float len = length(d);
                if (SRayHit hit; len > 1e-3f && m_world.collision.raycast(eye, d / len, len, hit))
                    k = std::min(k, std::max(0.05f, (hit.t - 0.02f) / len));
            }
            pl.center = eye + (pl.center - eye) * k;
            pl.scale *= k;
        }
        pl.targetCenter = pl.center;
        pl.targetRot    = pl.rot;
        pl.targetScale  = pl.scale;
        return "unpinned";
    }
    if (m_hold.key == key)
        m_hold = {};
    if (it == m_placements.end() || it->second.returning) {
        // from wherever it's drawn now (from the wall: the corner's pose takes over from there)
        const int i = windowPanel(w);
        if (i < 0)
            return "error: it isn't in the 3D view";
        const SPanel& p  = m_panels[i];
        SPlacement&   pl = m_placements[key];
        pl.window        = w;
        pl.center        = p.pose.at(p.box.size() * 0.5);
        pl.rot           = Quat::fromBasis(p.pose.right, -p.pose.down, p.pose.normal);
        pl.scale         = p.pose.scale;
        pl.returning     = false;
        it               = m_placements.find(key);
    }
    it->second.pinned = ++m_pinCount;
    return "pinned";
}

// H: the window you carry, else the one under the crosshair, pinned to your view; with one pinned, H puts the last
// one pinned down where it is (the crosshair never reaches a pinned window: it's in the view's corner, and the
// window under the crosshair would be pinned along with it)
std::string CDesktop3D::togglePin() {
    if (m_mode != MODE_ACTIVE)
        return "error: not in 3D";
    PHLWINDOW w;
    if (m_hold.key)
        if (const auto it = m_placements.find(m_hold.key); it != m_placements.end())
            w = it->second.window.lock();
    if (w)
        return setPinned(w, true);

    uint64_t last = 0;
    for (const auto& [key, pl] : m_placements) {
        const auto pw = pl.window.lock();
        if (pl.pinned > last && pw && windowPanel(pw) >= 0) { // (not one that isn't drawn: a hidden tab of a group)
            last = pl.pinned;
            w    = pw;
        }
    }
    if (w)
        return setPinned(w, false);

    if (m_menu.open()) // (the crosshair is hidden)
        return "error: close the Action Menu first";
    if (m_aimed >= 0 && m_aimed < (int)m_panels.size() && m_panels[m_aimed].kind != PANEL_LAYER)
        w = m_panels[m_aimed].window.lock();
    if (!w)
        return "error: point the crosshair at a window to pin it";
    return setPinned(w, true);
}

// its real size (the app draws itself anew at it), logical px: floating, if it was tiled (a window can only be any
// size that way). Its size in the world goes with it, text staying as big
std::string CDesktop3D::resizeReal(const PHLWINDOW& w, const Vector2D& size) {
    if (w->isFullscreen())
        return "error: it's fullscreen";
    const Vector2D want{std::round(std::clamp(size.x, 64.0, 8192.0)), std::round(std::clamp(size.y, 48.0, 8192.0))};
    if (!w->m_isFloating)
        Config::Actions::floatWindow(Config::Actions::TOGGLE_ACTION_ENABLE, w);
    if (const auto r = Config::Actions::resize(want, false, w); !r)
        return "error: " + r.error().message;
    return std::format("{:.0f}x{:.0f}", want.x, want.y);
}

// the Action Menu's Apps (the favourites, then all) and Windows pages, and a window's own
SMenuPage CDesktop3D::ownPage(const std::string& id) {
    SMenuPage p;
    // (an icon not loaded yet is loaded a few a frame, in update(), and shows once it is)
    const auto icon = [](const std::string& name) { return appIcon(name, ICON_PX, false); };
    if (id == "apps" || id == "apps/all") {
        const auto& all  = apps();
        const auto  item = [&](const SAppEntry& e) {
            return SMenuItem{.label = e.name, .hint = e.comment.size() > 28 ? "" : e.comment, .icon = "📦", .picture = icon(e.icon), .action = MA_LAUNCH, .target = e.id};
        };
        if (id == "apps") {
            p.title = "Apps";
            for (const auto& fav : commaList(g_cfgApps)) {
                if (const SAppEntry* e = findApp(all, fav))
                    p.items.push_back(item(*e));
                else
                    p.items.push_back({.label = fav.substr(0, fav.find(' ')), .hint = "command", .icon = "▶️", .action = MA_LAUNCH, .target = fav});
            }
            p.items.push_back({.label = "All apps", .hint = std::format("{}", all.size()), .icon = "📂", .page = "apps/all"});
        } else {
            p.title = "All apps";
            for (const auto& e : all)
                p.items.push_back(item(e));
        }
        return p;
    }
    if (id == "windows") {
        p.title = "Windows";
        for (const auto& w : g_pCompositor->m_windows) {
            if (!toplevel(w))
                continue;
            const auto        pl    = m_placements.find(reinterpret_cast<uintptr_t>(w.get()));
            const std::string where = pl == m_placements.end() || pl->second.returning ? (w->m_workspace ? "wall · " + w->m_workspace->m_name : "wall")
                : pl->second.pinned                                                   ? "pinned to the view"
                                                                                      : "in the world";
            const SAppEntry*  app   = appForClass(apps(), classOf(w));
            std::string       label = w->m_title.empty() ? classOf(w) : w->m_title;
            if (label.size() > 40)
                label = label.substr(0, 38) + "…";
            p.items.push_back({.label = label, .hint = where, .icon = "🪟", .picture = app ? icon(app->icon) : nullptr,
                               .page = std::format("win:{:x}", reinterpret_cast<uintptr_t>(w.get()))});
        }
        return p;
    }
    // win:ADDRESS: what to do with it
    const std::string addr = "0x" + id.substr(4);
    const auto        w    = findWindow(addr);
    if (!w)
        return {.title = "Gone"};
    const auto pl     = m_placements.find(reinterpret_cast<uintptr_t>(w.get()));
    const bool placed = pl != m_placements.end() && !pl->second.returning, pinned = placed && pl->second.pinned;
    const auto size   = w->m_realSize->goal();
    p.title           = w->m_title.empty() ? classOf(w) : w->m_title;
    if (p.title.size() > 30)
        p.title = p.title.substr(0, 28) + "…";
    p.items = {
        {.label = "Focus", .icon = "🎯", .action = MA_WINDOW, .arg = WA_FOCUS, .target = addr},
        {.label = "Bring here", .icon = "🫴", .action = MA_WINDOW, .arg = WA_BRING, .target = addr},
        {.label = "To the wall", .icon = "🧱", .action = MA_WINDOW, .arg = WA_WALL, .target = addr, .disabled = !placed},
        {.label = pinned ? "Unpin" : "Pin to view", .icon = "📌", .action = MA_WINDOW, .arg = WA_PIN, .target = addr, .on = pinned},
        {.label = "Bigger", .hint = std::format("{:.0f}×{:.0f}", size.x, size.y), .icon = "➕", .action = MA_WINDOW, .arg = WA_BIGGER, .target = addr},
        {.label = "Smaller", .icon = "➖", .action = MA_WINDOW, .arg = WA_SMALLER, .target = addr},
        {.label = "Play", .hint = "Super+Esc ends it", .icon = "🎮", .action = MA_WINDOW, .arg = WA_PLAY, .target = addr},
        {.label = "Close", .icon = "❌", .action = MA_WINDOW, .arg = WA_CLOSE, .target = addr},
    };
    return p;
}

std::string CDesktop3D::windowsStatus() const {
    std::string list;
    for (const auto& [key, pl] : m_placements) {
        const auto w = pl.window.lock();
        // (height: metres, the window's)
        list += std::format(R"({}{{"class": "{}", "title": "{}", "address": "0x{:x}", "center": [{:.3f}, {:.3f}, {:.3f}], "distance": {:.3f}, "size": {:.3f}, "height": {:.3f}, "held": {}, "pinned": {}, "settled": {}, "returning": {}}})",
                            list.empty() ? "" : ", ", jsonEscape(w ? w->m_class : ""), jsonEscape(w ? w->m_title : ""), key, pl.center.x, pl.center.y, pl.center.z,
                            length(pl.center - m_camera.eye), pl.scale / m_screen.scale(), w ? w->m_realSize->goal().y * pl.scale : 0.0, key == m_hold.key, pl.pinned != 0,
                            pl.settled, pl.returning);
    }
    const std::string hold = m_hold.key ? std::format(R"({{"dist": {:.3f}, "size": {:.3f}}})", m_hold.dist, m_hold.scaleMul) : "null";
    return std::format(R"({{"placed": [{}], "hold": {}, "spots": {}}})", list, hold, m_spots.size());
}

// ----------------------------------------------------------------- drawing

namespace {
    // How far down the top right corner Hyprland's own things reach on this monitor, output pixels (0 = none). They
    // show on the focused monitor only: the config error bar (errorOverlay/Overlay.cpp, at the top unless
    // debug:error_position is bottom), and the notifications, stacked from the top right under the reserved area
    // (notification/NotificationOverlay.cpp's drawNotifications; its NOTIF_OFFSET_Y, NOTIF_PAD_Y and NOTIF_GAP_Y, 10 px
    // each, aren't in a header). Their sizes are what Hyprland drew them at this frame, before our element.
    float hyprlandOverlaysBottom(const PHLMONITOR& mon) {
        if (!mon || mon != Desktop::focusState()->monitor())
            return 0;
        constexpr float OFFSET_Y = 10, PAD_Y = 10, GAP_Y = 10;
        static auto     PERRORPOS = CConfigValue<Config::INTEGER>("debug:error_position");
        float           bottom    = 0;
        if (ErrorOverlay::overlay()->active() && *PERRORPOS == 0)
            bottom = 10.f * (float)mon->m_scale + ErrorOverlay::overlay()->height(); // (its pad is 10 logical px)
        if (const auto notes = Notification::overlay()->getNotifications(); !notes.empty()) {
            float y = OFFSET_Y + (float)(mon->m_reservedArea.top() * mon->m_scale);
            for (const auto& n : notes)
                y += (float)std::max(n->m_cache.textSize.y, n->m_cache.iconSize.y) + PAD_Y + GAP_Y;
            bottom = std::max(bottom, y - GAP_Y);
        }
        return bottom;
    }
}

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
    f.aimed     = m_mode == MODE_ACTIVE && !m_menu.visible() && m_play.t <= 0.f ? m_aimed : -1;
    // (none in play mode, where the app's cursor is the only one; where the app's cursor shows, only its dot; none
    // while the mouse is away on another monitor)
    f.crosshair    = !m_menu.visible() && m_play.t <= 0.f && !m_away;
    f.crosshairDot = m_cursorShown;
    f.typing       = m_typing;
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
    m_badgeBox = {};
    if (m_mic.on()) { // while it listens, a badge says so (and what's wrong, if the microphone gives nothing): top right,
        // under Hyprland's notifications while they show
        const float scale = (float)mon->m_scale;
        if (scale != m_badgeScale || m_badge.empty()) {
            drawBadge(m_badge, m_badgeW, m_badgeH, m_badgeText.empty() ? "lip sync: listening" : m_badgeText, scale);
            m_badgeScale = scale;
            ++m_badgeSerial;
        }
        const float margin = 12.f * scale;
        const float top    = std::max(margin, std::min(hyprlandOverlaysBottom(mon) + margin, f.height - margin - m_badgeH));
        m_badgeBox         = {f.width - margin - m_badgeW, top, (double)m_badgeW, (double)m_badgeH};
        f.badge            = {.pixels = &m_badge, .w = m_badgeW, .h = m_badgeH, .serial = m_badgeSerial, .x = f.width - margin - m_badgeW / 2.f,
                              .y = top + m_badgeH / 2.f, .scale = 1, .alpha = 1};
    }

    const auto t0 = std::chrono::steady_clock::now();
    m_renderer.render(f, m_outTex->m_texID);
    m_renderMs = m_renderMs * 0.95f + 0.05f * std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();

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

std::string CDesktop3D::setLipSyncGain(const std::string& v) {
    std::string t = v;
    std::erase_if(t, [](char c) { return c == ' ' || c == '+'; });
    if (t.ends_with("dB") || t.ends_with("db"))
        t.resize(t.size() - 2);
    if (t.empty() || t == "auto" || t == "[[EMPTY]]") {
        m_lip.setGain(std::nullopt);
        return "auto";
    }
    char*       end = nullptr;
    const float dB  = std::strtof(t.c_str(), &end);
    if (!end || *end || !std::isfinite(dB))
        return "error: lip sync gain: a number of dB (-20 to 60), or auto";
    m_lip.setGain(dB);
    return std::format("{:.1f}", *m_lip.gainSetting());
}

void CDesktop3D::setLipSyncSource(const std::string& v) {
    const std::string source = v == "[[EMPTY]]" || v == "default" ? "" : v;
    if (source == m_lipsyncSource)
        return;
    m_lipsyncSource = source;
    if (m_mic.on()) { // listening: to that one now
        m_mic.stop();
        m_lip.reset();
        m_micWatch = {};
        m_badge.clear();
    }
    lipSync();
}

void CDesktop3D::lipSync() {
    const bool want = m_lipsync && m_mode != MODE_OFF && m_avatar;
    if (want && !m_mic.on()) {
        std::string error;
        if (!m_mic.start(error, m_lipsyncSource)) {
            m_lipsync = false;
            notify("lip sync: " + error, true);
            return;
        }
        m_lip.reset();
        m_micWatch = {};
        m_badgeText.clear();
        m_badge.clear();
    } else if (!want && m_mic.on()) {
        m_mic.stop();
        m_lip.reset();
        m_anim.setVisemes({});
        m_badgeText.clear();
        m_badge.clear();
        m_badgeBox = {};
    }
    if (!m_mic.on())
        return;
    m_micSamples.clear();
    int rate = 0;
    m_mic.read(m_micSamples, rate);
    m_lip.feed(m_micSamples.data(), m_micSamples.size(), rate);
    m_anim.setVisemes(m_lip.visemes());
    watchMicrophone();
}

namespace {
    // what can be wrong with the microphone, as the badge says it
    enum eMicProblem : uint8_t {
        MP_STARTING, // (nothing known yet)
        MP_NONE,
        MP_ERROR,    // PipeWire ended the stream (it went away)
        MP_UNLINKED, // nothing feeds it
        MP_MUTED,    // PipeWire has the source muted
        MP_SILENT,   // exact zeros: a microphone muted on itself (its button), or a device that sends nothing
        MP_NOTHING,  // no samples at all
        MP_MISSING,  // the microphone asked for isn't there: listening to the default one
    };

    bool micThere(const SMicStatus& s) { // the one asked for (none: the default)
        return s.target.empty() || std::ranges::any_of(s.sources, [&](const auto& src) { return src.first == s.target; });
    }

    std::string micName(const SMicStatus& s) {
        std::string n = !s.sourceNick.empty() ? s.sourceNick : !s.sourceDescription.empty() ? s.sourceDescription : s.sourceName;
        if (n.size() > 28) { // (whole letters)
            size_t cut = 26;
            while (cut > 0 && ((unsigned char)n[cut] & 0xC0) == 0x80)
                --cut;
            n = n.substr(0, cut) + "…";
        }
        return n.empty() ? "the microphone" : n;
    }

    std::string micBadge(int problem, const SMicStatus& s, const std::string& error) {
        switch (problem) {
            case MP_NONE: return std::format("lip sync: listening ({})", micName(s));
            case MP_ERROR: return "lip sync: PipeWire: " + error;
            case MP_MISSING: return std::format("lip sync: no {} (listening to {})", s.target, micName(s));
            case MP_UNLINKED: return "lip sync: no microphone linked";
            case MP_MUTED: return std::format("lip sync: {} is muted", micName(s));
            case MP_SILENT: return std::format("lip sync: no sound from {} (muted?)", micName(s));
            case MP_NOTHING: return std::format("lip sync: nothing from {}", micName(s));
            default: return "lip sync: listening";
        }
    }

    std::string micAdvice(int problem, const SMicStatus& s, const std::string& error) {
        switch (problem) {
            case MP_ERROR: return "lip sync: PipeWire ended it: " + error + ". It tries again every few seconds";
            case MP_MISSING: return std::format("lip sync: no microphone {} (lipsync_source), so {} instead; wpctl status lists them", s.target, micName(s));
            case MP_UNLINKED: return "lip sync: no microphone is linked to it (wpctl status lists them)";
            case MP_MUTED:
                return std::format("lip sync: {} is muted in PipeWire: wpctl set-mute {} 0, or your sound settings", micName(s), s.sourceId ? std::to_string(s.sourceId) : "@DEFAULT_AUDIO_SOURCE@");
            case MP_SILENT: return std::format("lip sync: {} sends only silence: is it muted, maybe by its own button?", micName(s));
            case MP_NOTHING: return std::format("lip sync: nothing comes from {} (PipeWire has it {})", micName(s), s.sourceState.empty() ? "unknown" : s.sourceState);
            default: return "";
        }
    }
}

void CDesktop3D::watchMicrophone() {
    const auto now = std::chrono::steady_clock::now();
    const float dt = m_micWatch.looked.time_since_epoch().count() ? std::chrono::duration<float>(now - m_micWatch.looked).count() : 0.f;
    if (m_micWatch.problem >= 0 && dt < 0.25f) // (four times a second)
        return;
    m_micWatch.looked = now;
    SMicStatus st     = m_mic.status();
    // broken (PipeWire went away, or ended it: a microphone asked for that isn't there): open it again every few
    // seconds, so a microphone plugged in (or PipeWire back) brings lip sync back
    if (st.stream == "error" || st.stream == "unconnected") {
        if (!st.error.empty())
            m_micWatch.error = st.error;
        if ((m_micWatch.broken += dt) >= 3.f) {
            m_micWatch.broken = 0;
            m_mic.stop();
            std::string error;
            if (!m_mic.start(error, m_lipsyncSource))
                m_micWatch.error = error;
            st = m_mic.status();
        }
    } else
        m_micWatch.broken = 0;
    if (st.linked)
        m_micWatch.error.clear();
    if (st.linked && st.sourceName != m_micWatch.source)
        log(std::format("lip sync: listening to {} ({}, node {})", st.sourceName, st.sourceDescription, st.sourceId));
    m_micWatch.source = st.linked ? st.sourceName : "";

    int problem = MP_STARTING;
    if (st.linked)
        problem = st.muted == 1                                           ? MP_MUTED :
            st.silentFor >= 2                                             ? MP_SILENT :
            (st.sinceData < 0 && st.age >= 2) || st.sinceData >= 2 ? MP_NOTHING :
            !micThere(st)                                                 ? MP_MISSING :
                                                                            MP_NONE;
    else if (!m_micWatch.error.empty())
        problem = MP_ERROR;
    else if (st.age >= 1.5)
        problem = MP_UNLINKED;
    const std::string text = micBadge(problem, st, m_micWatch.error);
    if (text != m_badgeText) { // drawn anew
        if (!m_badgeText.empty() || problem != MP_STARTING)
            log(text);
        m_badgeText = text;
        m_badge.clear();
    }
    if (problem != m_micWatch.problem && problem > MP_NONE && !m_micWatch.notified) { // once each time it listens
        m_micWatch.notified = true;
        notify(micAdvice(problem, st, m_micWatch.error), true);
    }
    m_micWatch.problem = problem;
}

std::string CDesktop3D::lipSyncStatus() const {
    const auto&       v     = m_lip.visemes();
    const std::string badge = m_mic.on() && !m_badgeBox.empty() ? std::format("[{:.0f}, {:.0f}, {:.0f}, {:.0f}]", m_badgeBox.x, m_badgeBox.y, m_badgeBox.w, m_badgeBox.h) : "null";
    const SMicStatus  st    = m_mic.status();
    auto dBorNull = [](float x) { return x <= -199.f ? std::string("null") : std::format("{:.1f}", x); };
    std::string source = "null";
    if (st.linked)
        source = std::format(R"({{"id": {}, "name": "{}", "description": "{}", "nick": "{}", "state": "{}", "muted": {}, "volume": {}}})", st.sourceId, jsonEscape(st.sourceName),
                             jsonEscape(st.sourceDescription), jsonEscape(st.sourceNick), jsonEscape(st.sourceState), st.muted < 0 ? "null" : st.muted ? "true" : "false",
                             st.volume < 0 ? std::string("null") : std::format("{:.3f}", st.volume));
    std::string sources;
    for (const auto& [name, description] : st.sources)
        sources += std::format(R"({}{{"name": "{}", "description": "{}"}})", sources.empty() ? "" : ", ", jsonEscape(name), jsonEscape(description));
    static constexpr const char* PROBLEMS[] = {"starting", "none", "error", "unlinked", "muted", "silent", "nothing", "missing"};
    const auto   marks = m_lip.marks();
    const auto   set   = m_lip.gainSetting();
    return std::format(
        R"({{"on": {}, "listening": {}, "microphone": {}, "level": {:.1f}, "formants": [{:.0f}, {:.0f}], "visemes": {{"aa": {:.2f}, "ih": {:.2f}, "ou": {:.2f}, "ee": {:.2f}, "oh": {:.2f}, "pp": {:.2f}, "ff": {:.2f}, "ss": {:.2f}, "ch": {:.2f}}}, "badge": {}, )"
        R"("text": "{}", "problem": "{}", "stream": "{}", "error": "{}", "coreError": "{}", "linked": {}, "source": {}, "target": "{}", "samples": {}, "buffers": {}, "emptyBuffers": {}, "silentFor": {:.2f}, )"
        R"("sinceData": {}, "peak": {}, "rms": {}, "gain": {:.1f}, "gainSetting": {}, "reference": {:.1f}, "room": {}, "marks": [{:.1f}, {:.1f}], "sources": [{}]}})",
        m_lipsync, m_mic.on(), CMicrophone::available(), m_lip.level(), m_lip.f1(), m_lip.f2(), v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], badge,
        jsonEscape(m_mic.on() ? m_badgeText : ""), m_mic.on() && m_micWatch.problem >= 0 ? PROBLEMS[m_micWatch.problem] : "", jsonEscape(st.stream),
        jsonEscape(!st.error.empty() ? st.error : m_micWatch.error), jsonEscape(st.coreError), st.linked, source, jsonEscape(st.target), st.samples, st.buffers, st.emptyBuffers, st.silentFor,
        st.sinceData < 0 ? std::string("null") : std::format("{:.2f}", st.sinceData), dBorNull(st.peak), dBorNull(st.rms), m_lip.gain(),
        set ? std::format("{:.1f}", *set) : std::string("\"auto\""), m_lip.reference(), std::isnan(m_lip.room()) ? std::string("null") : std::format("{:.1f}", m_lip.room()),
        marks[0], marks[1], sources);
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
        case MA_LAUNCH: {
            const std::string r = launch(it.target);
            if (!r.starts_with("error: "))
                m_menu.hide(); // (to see it open)
            return r;
        }
        case MA_WINDOW: {
            const auto        a = (eWindowAction)it.arg;
            const std::string r = windowAction(findWindow(it.target), a);
            if (!r.starts_with("error: ") && (a == WA_FOCUS || a == WA_BRING || a == WA_PLAY || a == WA_CLOSE))
                m_menu.hide();
            return r;
        }
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
    if (args[0] == "open" || (args[0] == "toggle" && !m_menu.open())) {
        if (m_away)
            setAway(false); // (it wants the mouse and keyboard: back into 3D from another monitor)
        setTyping(false);
    }
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
    // the app's cursor as it's drawn: where on the panel, how big and where its hotspot is
    std::string cursor = "null";
    if (m_cursorShown) {
        const auto size = g_pPointerManager->cursorSizeLogical();
        const auto hot  = g_pPointerManager->hotspot();
        cursor = std::format(R"({{"at": [{:.1f}, {:.1f}], "size": [{:.0f}, {:.0f}], "hotspot": [{:.0f}, {:.0f}]}})", m_pointerAt.local.x, m_pointerAt.local.y, size.x, size.y, hot.x, hot.y);
    }
    return std::format(
        R"({{"mode": "{}", "monitor": "{}", "away": {}, "view": "{}", "typing": {}, "playing": {}, "cursor": {}, "fly": {}, "feet": [{:.3f}, {:.3f}, {:.3f}], "eye": [{:.3f}, {:.3f}, {:.3f}], "yaw": {:.2f}, "pitch": {:.2f}, "onGround": {}, "panels": {}, "aimed": {}, "fps": {:.1f}, "frames": {}, "minDt": {:.5f}, "updateMs": {:.2f}, "renderMs": {:.2f}, "sens": {}, "placed": {}, "holding": {}, "world": "{}", "map": "{}", "mapLoading": {}, "avatar": "{}", "avatarLoading": {}, "anim": "{}", "exposure": {:.2f}, "light": {:.3f}, "menu": "{}", "hooks": {{"motion": {}, "warp": {}, "cursor": {}, "wheel": {}, "frame": {}, "softCursor": {}, "discard": {}}}}})",
        modes[m_mode], m_mode != MODE_OFF && m_monitor.lock() ? jsonEscape(m_monitor.lock()->m_name) : "", m_mode != MODE_OFF && m_away, m_thirdPerson ? "third" : "first", m_typing, playStatus(), cursor, m_fly, m_feet.x, m_feet.y, m_feet.z, m_camera.eye.x, m_camera.eye.y, m_camera.eye.z, m_yaw * 180.f / F_PI, m_pitch * 180.f / F_PI, m_onGround,
        m_panels.size(), aimed, m_fps, m_frames, m_minDt, m_updateMs, m_renderMs, m_sens, m_placements.size(), m_hold.key != 0, jsonEscape(m_world.name), jsonEscape(m_mapPath), m_mapLoader.busy(),
        jsonEscape(m_avatarPath), m_avatarLoader.busy(), jsonEscape(m_anim.playing()), m_exposure, m_lightAvg, m_menu.open() ? jsonEscape(m_menu.path()) : "", m_hookMoved != nullptr, m_hookWarp != nullptr, m_hookCursor != nullptr, m_hookWheel != nullptr, m_hookFrame != nullptr, m_hookSoftCursor != nullptr, m_hookDiscard != nullptr);
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
    if (cmd == "on") { // on [MONITOR]: 3D on that one, else on plugin:hypr3d:monitor's, else on the focused one
        PHLMONITOR mon;
        if (args.size() > 1) {
            std::string name = args[1];
            for (size_t i = 2; i < args.size(); ++i) // (a description has spaces)
                name += " " + args[i];
            mon = monitorNamed(name);
            if (!mon)
                return "error: no monitor " + name + " is connected (hyprctl monitors lists them)";
            if (const auto now = m_monitor.lock(); m_mode != MODE_OFF && now && now != mon)
                return "error: in 3D on " + now->m_name + " already";
        }
        return enter(mon) ? "ok" : "error: can't enter 3D right now";
    }
    if (cmd == "off") {
        exit(args.size() > 1 && args[1] == "now");
        return "ok";
    }
    if (cmd == "away") { // away [on|off|toggle]: the mouse and keyboard to the desktop on another monitor, or back
        const std::string v = args.size() > 1 ? args[1] : "toggle";
        if (v != "on" && v != "off" && v != "toggle")
            return "error: away [on|off|toggle]";
        return setAway(v == "on" || (v == "toggle" && !m_away));
    }
    if (cmd == "type") {
        setTyping(args.size() > 1 ? args[1] != "off" : !m_typing);
        return m_play.on ? "playing" : m_typing ? "typing" : "walking";
    }
    if (cmd == "play") { // play [on|off|toggle]: the window under the crosshair gets everything; without one, what's played
        const std::string v = args.size() > 1 ? args[1] : "";
        if (v.empty())
            return playStatus();
        if (v != "on" && v != "off" && v != "toggle")
            return "error: play [on|off|toggle]";
        return setPlay(v == "on" || (v == "toggle" && !m_play.on));
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
    if (cmd == "aim") { // aim [window]: turn to face a window's middle: that one (an address, a class or a title), else the one
        // nearest to where you look (when the crosshair is on the wallpaper between them, say)
        if (m_mode != MODE_ACTIVE)
            return "error: not in 3D";
        const PHLWINDOW want = args.size() > 1 ? findWindow(args[1]) : nullptr;
        if (args.size() > 1 && !want)
            return "error: no window " + args[1];
        const V3      fwd  = forwardFrom(m_yaw, m_pitch);
        const SPanel* best = nullptr;
        float         most = -2.f;
        V3            to;
        for (const auto& p : m_panels) {
            const auto w = p.kind == PANEL_WINDOW ? p.window.lock() : nullptr;
            if (!w || (want && w != want) || p.alpha < 0.5f)
                continue;
            const V3 c = p.pose.at({p.box.w / 2, p.box.h / 2}), d = normalize(c - m_camera.eye);
            if (dot(d, p.pose.normal) > -0.05f) // (from behind, or edge on)
                continue;
            if (const float k = dot(d, fwd); k > most) {
                most = k;
                best = &p;
                to   = c - m_camera.eye;
            }
        }
        if (!best)
            return "error: no window to aim at";
        m_yaw   = wrapAngle(std::atan2(to.x, -to.z));
        m_pitch = std::clamp(std::atan2(to.y, std::hypot(to.x, to.z)), -1.55f, 1.55f);
        const auto w = best->window.lock();
        return std::format("0x{:x} {}", (uintptr_t)w.get(), w->m_class);
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
    if (cmd == "pin") // pin the window carried or under the crosshair to your view, or put the last one pinned down, like H
        return togglePin();
    if (cmd == "hold") { // hold <distance> [scale]: while holding, set how far away and how big
        if (!m_hold.key)
            return "error: not holding anything";
        m_hold.dist     = std::clamp(num(1, m_hold.dist), 0.6f, 12.f);
        m_hold.scaleMul = std::clamp(num(2, m_hold.scaleMul), 0.2f, 5.f);
        return "ok";
    }
    if (cmd == "reset-windows") { // everything back on the desktop wall; forget: nor go anywhere else again
        m_hold = {};
        for (auto& [key, pl] : m_placements) {
            pl.returning = true;
            pl.pinned    = 0;
        }
        if (args.size() > 1 && args[1] == "forget") {
            loadSpots();
            m_spots.clear();
            saveWindowSpots(m_mapPath, m_spots);
        }
        return "ok";
    }
    if (cmd == "panels") { // everything drawn in 3D, in drawing order: its kind, window, box on the desktop, state, surfaces
        static constexpr const char* KINDS[] = {"layer", "window", "popup"};
        std::string                  list;
        for (size_t i = 0; i < m_panels.size(); ++i) {
            const auto& p = m_panels[i];
            const auto  w = p.window.lock();
            std::string surfaces; // (on the desktop too; the app's cursor isn't one)
            for (const auto& sf : p.surfaces)
                if (sf.surface)
                    surfaces += std::format("{}[{:.0f}, {:.0f}, {:.0f}, {:.0f}]", surfaces.empty() ? "" : ", ", p.box.x + sf.box.x, p.box.y + sf.box.y, sf.box.w, sf.box.h);
            list += std::format(R"({}{{"kind": "{}", "class": "{}", "title": "{}", "box": [{:.0f}, {:.0f}, {:.0f}, {:.0f}], "placed": {}, "front": {}, "aimed": {}, "alpha": {:.2f}, "surfaces": [{}]}})",
                                list.empty() ? "" : ", ", KINDS[p.kind], jsonEscape(w ? w->m_class : ""), jsonEscape(w ? w->m_title : ""), p.box.x, p.box.y, p.box.w, p.box.h,
                                p.placed, p.front, (int)i == m_aimed, p.alpha, surfaces);
        }
        return "[" + list + "]";
    }
    if (cmd == "windows") // the windows off the wall: where, how far from the eye, how big (1 = as on the wall), pinned
        return windowsStatus();
    if (cmd == "log") // log [lines]: what the plugin logged lately (Hyprland's own log has it only with its debug logs on)
        return logLines(args.size() > 1 ? (size_t)std::max(1, std::atoi(args[1].c_str())) : 400);
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

    if (cmd == "launch") { // launch <desktop id|name|command>: in front of you in 3D
        const std::string what = unquote(afterWords(2));
        return what.empty() ? "error: launch <desktop id, name or command>" : launch(what);
    }
    if (cmd == "apps") { // the desktop entries: id, name, what it runs, its icon found
        std::string list;
        for (const auto& e : apps())
            list += std::format(R"({}{{"id": "{}", "name": "{}", "exec": "{}", "class": "{}", "icon": {}}})", list.empty() ? "" : ", ", jsonEscape(e.id), jsonEscape(e.name),
                                jsonEscape(e.exec), jsonEscape(e.wmClass), appIcon(e.icon, ICON_PX) != nullptr);
        return "[" + list + "]";
    }
    if (cmd == "window") { // window <address|class|title> focus|bring|wall|pin|unpin|bigger|smaller|size W H|play|close
        if (args.size() < 3)
            return "error: window <address|class|title> focus|bring|wall|pin|unpin|bigger|smaller|size W H|play|close";
        const auto w = findWindow(args[1]);
        if (!w)
            return "error: no window " + args[1];
        const std::string& a = args[2];
        static const std::unordered_map<std::string, eWindowAction> ACTIONS = {{"focus", WA_FOCUS},     {"bring", WA_BRING},     {"wall", WA_WALL},
                                                                               {"bigger", WA_BIGGER},   {"smaller", WA_SMALLER}, {"play", WA_PLAY},
                                                                               {"close", WA_CLOSE}};
        if (a == "pin" || a == "unpin")
            return setPinned(w, a == "pin");
        if (a == "size")
            return resizeReal(w, {num(3, 0), num(4, 0)});
        const auto it = ACTIONS.find(a);
        return it == ACTIONS.end() ? "error: no such window action: " + a : windowAction(w, it->second);
    }
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
        if (sub == "lipsync") { // avatar lipsync [on|off|toggle|gain dB|auto|source name|default]
            const std::string v = args.size() > 2 ? args[2] : "";
            if (v == "on" || v == "off" || v == "toggle") {
                if (const std::string r = setLipSync(v == "on" || (v == "toggle" && !m_lipsync)); r.starts_with("error"))
                    return r;
            } else if (v == "gain" && args.size() > 3) {
                if (const std::string r = setLipSyncGain(unquote(afterWords(4))); r.starts_with("error"))
                    return r;
            } else if (v == "source" && args.size() > 3)
                setLipSyncSource(unquote(afterWords(4)));
            else if (!v.empty())
                return "error: avatar lipsync [on|off|toggle|gain dB|auto|source name|default]";
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
    return "usage: hyprctl hypr3d [status|toggle|on [monitor]|off [now]|away [on|off|toggle]|type [on|off]|play [on|off|toggle]|look dx dy|turn yaw pitch|tp x y z|walk secs [forward|back|left|right]|jump|fly|click "
           "[left|right|middle]|sens [value]|aim [window]|grab|place|hold dist [scale]|pin|reset-windows [forget]|windows|panels|log [lines]|launch what|apps|window sel action|map [path|none|reload|forget|scale s]|spawn [here]|desktop [here [height]]|"
           "avatar [path|none|reload|height m|expression [name [weight]|none]|gesture [left|right|both gesture]|parts [reset]|toggle name [on|off|reset]|"
           "shape name [weight|reset]|physics [on|off|toggle]|emote [name|number|file|folder [once|loop]|stop]|lipsync [on|off|toggle|gain dB|auto|source name|default]]|"
           "view [first|third|toggle] [distance] [side]|"
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
    int luaPlay(lua_State*) {
        if (g_p3D)
            g_p3D->setPlay(!g_p3D->playing());
        return 0;
    }
    int luaAway(lua_State*) {
        if (g_p3D)
            g_p3D->setAway(!g_p3D->away());
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
    // plugin { hypr3d { lipsync_gain = auto, lipsync_source = "" } }
    g_cfgLipSyncGain = makeShared<Config::Values::CStringValue>("plugin:hypr3d:lipsync_gain",
                                                                "lip sync: how much louder the microphone counts, dB (-20 to 60), or auto: it goes by your voice", "auto");
    g_cfgLipSyncSource = makeShared<Config::Values::CStringValue>("plugin:hypr3d:lipsync_source",
                                                                  "lip sync: the microphone, by its name or description (wpctl status), \"\" = the default one", "");
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgLipSyncGain);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgLipSyncSource);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatar);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatarHeight);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatarPhysics);
    g_cfgAvatarEmotes = makeShared<Config::Values::CStringValue>("plugin:hypr3d:avatar_emotes",
                                                                 "more emotes: VRM animations (.vrma) or glTF clips, files or folders separated by commas", "");
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatarEmotes);
    // plugin { hypr3d { apps = firefox, discord, obs; app_rules = steam_app_.*: 2.2 1.5, discord: 1.3 0.8 left } }
    g_cfgApps     = makeShared<Config::Values::CStringValue>("plugin:hypr3d:apps", "the Apps page's favourites: desktop ids, names or commands, separated by commas", "");
    g_cfgAppRules = makeShared<Config::Values::CStringValue>("plugin:hypr3d:app_rules",
                                                             "where apps launched from 3D open: CLASS: DISTANCE HEIGHT [left|right|SIDE], separated by commas", "");
    g_cfgPinSize  = makeShared<Config::Values::CFloatValue>("plugin:hypr3d:pin_size", "how much of the view's height a window pinned to it takes", 0.3f,
                                                           Config::Values::SFloatValueOptions{.min = 0.05f, .max = 1.f});
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgApps);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAppRules);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgPinSize);
    // plugin { hypr3d { monitor = DP-1 } }: 3D on that monitor, the others staying the desktop
    g_cfgMonitor = makeShared<Config::Values::CStringValue>("plugin:hypr3d:monitor",
                                                            "the monitor 3D goes on: its name (DP-1) or desc: and its description; \"\" = the focused one", "");
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgMonitor);

    g_p3D = std::make_unique<CDesktop3D>();
    g_p3D->init();

    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "toggle", luaToggle);
    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "enter", luaEnter);
    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "exit", luaExit);
    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "type", luaType);
    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "play", luaPlay);
    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "away", luaAway);
    HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "menu", luaMenu);

    log("loaded");
    return {"hypr3d", "Walk around your desktop in first person", "hypr3d", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_p3D)
        g_p3D->shutdown();
    g_p3D.reset();
}
