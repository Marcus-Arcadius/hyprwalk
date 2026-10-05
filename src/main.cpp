// hyprwalk: walk around your Hyprland desktop in first person, its windows drawn as panels in a 3D world

#include "apps.hpp"
#include "compat.hpp"
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
#include "speaker.hpp"
#include "tiling.hpp"
#include "walker.hpp"
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
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/errorOverlay/Overlay.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/notification/NotificationOverlay.hpp>
#include <hyprland/src/protocols/PointerConstraints.hpp>
#include <hyprland/src/protocols/RelativePointer.hpp>
#include <hyprland/src/protocols/core/DataDevice.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/xwayland/XWayland.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <aquamarine/backend/Backend.hpp>

#include <array>
#include <bit>
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

namespace hyprwalk {
    namespace {
        // recent log lines for hyprctl hyprwalk log (Hyprland logs only with debug:disable_logs off)
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
        Log::logger->log(Log::INFO, "[hyprwalk] {}", s);
    }

    void notify(const std::string& s, bool error) {
        if (error) {
            remember("ERR", s);
            Log::logger->log(Log::ERR, "[hyprwalk] {}", s); // INFO is hidden unless debug logs are on
        } else
            log(s);
        if (PHANDLE)
            HyprlandAPI::addNotification(PHANDLE, "[hyprwalk] " + s, error ? CHyprColor{1.0, 0.35, 0.35, 1.0} : CHyprColor{0.45, 0.8, 1.0, 1.0}, error ? 8000 : 4000);
    }

    std::string logLines(size_t n) {
        std::lock_guard lock(g_logMutex);
        std::string     out;
        for (size_t i = g_logLines.size() > n ? g_logLines.size() - n : 0; i < g_logLines.size(); ++i)
            out += g_logLines[i] + "\n";
        return out;
    }
}

using namespace hyprwalk;

namespace {
    PHLWINDOW   parentOf(const PHLWINDOW& w);
    std::string classOf(const PHLWINDOW& w);
    int         keyHand(uint32_t key);

    constexpr float F_PI            = std::numbers::pi_v<float>;
    constexpr float FOV_Y         = 70.f * F_PI / 180.f;
    constexpr float ENTER_TIME    = 0.8f;
    constexpr float EXIT_TIME     = 0.6f;
    constexpr float PLAY_TIME     = 0.35f;
    constexpr float PLAY_FILL     = 0.94f; // of the view
    // play_size: default, Super+wheel step, limits
    constexpr float PLAY_SIZE      = 0.5f;
    constexpr float PLAY_SIZE_STEP = 0.05f;
    constexpr float PLAY_SIZE_MIN  = 0.25f;
    constexpr float PLAY_SIZE_MAX  = PLAY_FILL;
    constexpr float PLAY_SIZE_TELL = 0.5f; // notify after this long without a notch
    constexpr float FRONT_FIT     = 0.85f; // view fraction a new window fits in
    constexpr float THIRD_FIT     = 0.6f;  // ... in third person
    constexpr float WHEEL_SIZE    = 0.95f; // per wheel notch
    constexpr float CARRY_NEAREST = 0.6f;
    constexpr float TILE_RADIUS   = 2.f;
    constexpr float TILE_PAST     = 1.f;   // third person: past the camera boom
    constexpr float TILE_MIN_PX   = 100;
    constexpr float TILE_REPULL   = 0.05f; // ring move before re-checking obstacles
    constexpr int   TILE_REPULLS  = 2;     // windows re-checked a frame at most
    constexpr int   ICON_PX       = 96;
    // held game keys go back to walking after this long idle (s)
    constexpr float PLAY_PAUSE_IDLE = 4.f;
    // after a shortcut's window closes (within CLOSE_TIME s), hold shortcuts till you turn, move or HOLD_TIME s pass
    constexpr float SHORTCUT_CLOSE_TIME = 30.f;
    constexpr float SHORTCUT_HOLD_TURN = 2.f * F_PI / 180.f;
    constexpr float SHORTCUT_HOLD_MOVE = 0.3f;
    constexpr float SHORTCUT_HOLD_TIME = 5.f;

    constexpr float RADIUS        = SWalker::RADIUS;
    constexpr float HEIGHT        = 1.8f;
    constexpr float HEIGHT_CROUCH = 1.2f;
    constexpr float EYE           = 1.65f;
    constexpr float EYE_CROUCH    = 1.1f;
    constexpr float GRAVITY       = 20.f;
    constexpr float JUMP_SPEED    = 6.3f;
    constexpr float WALK_SPEED    = 1.6f; // default walk_speed
    constexpr float RUN_SPEED     = 4.5f; // default run_speed
    // default emote_volume: -6 dB (dance songs are mastered loud)
    constexpr float EMOTE_VOLUME  = 0.5f;
    constexpr float CROUCH_SPEED  = 1.f;
    constexpr float FLY_SPEED     = 8.f;
    constexpr float ACCEL         = 10.f; // m/s²
    constexpr float DECEL         = 14.f;
    constexpr float TURN_BACK     = 7.f;  // m/s² when reversing

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
        K_T      = 20,
        K_Y      = 21,
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
        K_F4     = 62,
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

    bool fromXWayland(const SP<CWLSurfaceResource>& surf) {
        return surf && g_pXWayland && g_pXWayland->m_server && surf->client() == g_pXWayland->m_server->m_xwaylandClient;
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

    // still walk while a game's keys are held back
    bool walkKey(uint32_t k) {
        return k == K_W || k == K_A || k == K_S || k == K_D || k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT || k == K_SPACE || k == K_LSHIFT || k == K_LCTRL ||
            k == K_C;
    }

    bool modifierKey(uint32_t k) {
        return k == K_LMETA || k == K_RMETA || k == K_LALT || k == K_RALT || k == K_LCTRL || k == K_RCTRL || k == K_LSHIFT || k == K_RSHIFT;
    }

    // cuts at a UTF-8 boundary, adding "…"
    std::string clipped(const std::string& s, size_t n) {
        if (s.size() <= n)
            return s;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
            --n;
        return s.substr(0, n) + "…";
    }

    // at most 8 hops: X11 WM_TRANSIENT_FOR can loop
    PHLWINDOW topParent(PHLWINDOW w) {
        for (int hops = 0; w && hops < 8; ++hops) {
            const auto up = parentOf(w);
            if (!up)
                break;
            w = up;
        }
        return w;
    }

}

constexpr const char* C3D_PASS_NAME = "CHyprwalkElement";

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

// a window taken off the desktop wall into the world
struct SPlacement {
    PHLWINDOWREF window;
    V3           center, targetCenter;
    Quat         rot, targetRot;       // x right, y up, z out of the front
    float        scale = 1, targetScale = 1; // meters per logical px
    bool         returning = false;    // flying back to the wall
    bool         settled   = false;    // pose reached the target
    uint64_t     pinned    = 0;        // pin order, 0 = not pinned
    // scale and eye distance before pinning (0 = unknown)
    float        pinnedFromScale = 0, pinnedFromDist = 0;
    bool         tiled   = false; // laid out by the tiling ring
    uintptr_t    follows = 0;     // dialog: the parent it sits in front of
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
    SP<Config::Values::CBoolValue>   g_cfgFirstPersonBody;
    SP<Config::Values::CBoolValue>   g_cfgLipSync;
    SP<Config::Values::CStringValue> g_cfgLipSyncGain;   // dB, or auto
    SP<Config::Values::CStringValue> g_cfgLipSyncSource; // "" = default
    SP<Config::Values::CStringValue> g_cfgAvatarEmotes;
    SP<Config::Values::CFloatValue>  g_cfgEmoteVolume; // 0 = muted
    SP<Config::Values::CStringValue> g_cfgApps;     // Apps page favourites
    SP<Config::Values::CStringValue> g_cfgAppRules; // where apps launched from 3D open
    SP<Config::Values::CFloatValue>  g_cfgPinSize;  // fraction of the view's height
    SP<Config::Values::CStringValue> g_cfgMonitor;  // "" = the focused one
    SP<Config::Values::CFloatValue>  g_cfgWalkSpeed; // m/s
    SP<Config::Values::CFloatValue>  g_cfgRunSpeed;
    SP<Config::Values::CBoolValue>   g_cfgTiling;    // on at start (T)
    SP<Config::Values::CBoolValue>   g_cfgTilingFollow; // ring follows you (Y)
    SP<Config::Values::CStringValue> g_cfgPlayView;     // here or fill (P); Shift+P the other
    SP<Config::Values::CFloatValue>  g_cfgPlaySize;     // view fraction when played here in the ring

    // config path, ~ expanded; "" when unset
    std::string configuredPath(const SP<Config::Values::CStringValue>& value) {
        std::string v = value ? value->value() : "";
        if (v == "[[EMPTY]]")
            v.clear();
        if (v.starts_with("~/"))
            if (const char* home = getenv("HOME"))
                v = home + v.substr(1);
        return v;
    }

    // trims, unquotes and expands ~
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

    // emote files and folders, comma-separated
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

    // "" = the built-in courtyard
    std::string configuredMap() {
        return configuredPath(g_cfgMap);
    }

    float layerSpacing() {
        const float v = g_cfgSpacing ? g_cfgSpacing->value() : 0.f;
        return v > 0.f ? std::min(v, 0.5f) : 0.02f;
    }

    // play_view lower-cased, "" when unset
    std::string configuredPlayView() {
        std::string v = g_cfgPlayView ? unquote(g_cfgPlayView->value()) : "";
        std::ranges::transform(v, v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return v == "[[empty]]" ? "" : v;
    }

    // anything but fill plays here
    bool configuredPlayFill() {
        return configuredPlayView() == "fill";
    }

    std::string configuredMonitor() {
        const std::string v = g_cfgMonitor ? unquote(g_cfgMonitor->value()) : "";
        return v == "[[EMPTY]]" ? "" : v;
    }

    // by name (DP-1) or desc:prefix, like monitor rules; null if not connected
    PHLMONITOR monitorNamed(const std::string& name) {
        const std::string v = unquote(name);
        if (v.empty())
            return nullptr;
        for (const auto& m : hypr::monitors())
            if (m && m->m_output && m->matchesStaticSelector(v))
                return m;
        return nullptr;
    }

    // entering 3D: unfocus an unseen window on another monitor (Super+Q would close it)
    bool unfocusOthers(const PHLMONITOR& mon) {
        if (const auto w = Desktop::focusState()->window(); !w || w->m_monitor.lock() == mon)
            return false;
        Desktop::focusState()->rawWindowFocus(nullptr, Desktop::FOCUS_REASON_OTHER);
        return true;
    }
}

class CDesktop3D {
  public:
    void                          init();
    void                          shutdown();

    bool                          enter(PHLMONITOR mon = nullptr);
    void                          exit(bool immediate = false);
    void                          toggle();
    std::string                   setTyping(bool on);
    // Super+Esc: input to another monitor, the 3D view staying up; false = back
    std::string                   setAway(bool away);
    bool                          away() const {
        return m_away;
    }

    std::string                   hyprctl(const std::string& request);
    std::string                   menuCommand(const std::vector<std::string>& args);
    std::string                   menuDispatch(const std::string& arg); // nothing = toggle, a page = open it, else a command

    // P: all input to the aimed window (a game), here or filling the view; nullopt = play_view
    std::string                   setPlay(bool on, std::optional<bool> fill = std::nullopt);
    std::string                   playDispatch(const std::string& arg);
    bool                          inOwnView(const V3& at) const;
    void                          turnTo(const V3& at);
    void                          checkPlayConfig();
    bool                          playing() const {
        return m_play.on;
    }

    // T: tiling mode, windows in a ring round you (tiling.cpp)
    std::string                   setTiling(bool on);
    std::string                   tileHere();
    std::string                   setTileFollow(bool on);
    std::string                   tileDispatch(const std::string& arg);
    bool                          tiling() const {
        return m_tiling.on;
    }

    // hooks, return true when the event was eaten
    bool                          onRelativeMotion(const IPointer::SMotionEvent& e);
    bool                          onAbsoluteMotion(const Vector2D& abs);
    void                          onPointerFrame();
    bool                          holdsPointer() const {
        return m_mode != MODE_OFF && !m_away;
    }
    // drawn in 3D this frame: presented, not "discarded"
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
    float                                 m_updateMs = 0, m_renderMs = 0; // averaged CPU ms a frame

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
    std::string          m_mapPath;          // shown or loading, "" = the courtyard
    std::string          m_mapConfigured;    // last value seen in the config
    float                m_mapScale = 0;

    // avatar
    CAvatarLoader                 m_avatarLoader;
    wl_event_source*              m_avatarSource = nullptr;
    std::string                   m_avatarPath;       // shown or loading, "" = none
    std::string                   m_avatarConfigured;
    int                           m_physicsConfigured = -1;
    float                         m_avatarHeight = 0; // 0 = the model's own
    std::shared_ptr<SAvatarModel> m_avatar;
    CAvatarAnimator               m_anim;
    CAvatarControl                m_ctl{m_anim};
    // lip sync on: the mic listens only in 3D with an avatar
    bool                          m_lipsync = false;
    int                           m_lipsyncConfigured = -1;
    CMicrophone                   m_mic;
    CLipSync                      m_lip;
    std::vector<float>            m_micSamples;
    std::string                   m_lipsyncSource;                  // "" = default
    std::string                   m_lipsyncSourceConfigured = "\n";
    std::string                   m_lipsyncGainConfigured   = "\n";
    // emote sound: what it was started for, and the last error
    CSpeaker                      m_speaker;
    struct {
        int           emote  = -1;
        uint32_t      starts = 0;
        const SSound* sound  = nullptr;
    }                             m_soundFor;
    std::string                   m_soundError;
    // microphone state for the badge, notifications and hyprctl
    struct {
        int         problem  = -1;    // eMicProblem
        bool        notified = false; // this listening session
        std::chrono::steady_clock::time_point looked; // last
        float       broken = 0;       // seconds broken, to reopen
        std::string error;            // last error, until relinked
        std::string source;           // last linked node.name
    } m_micWatch;
    std::vector<uint32_t>         m_badge;
    std::string                   m_badgeText;
    int                           m_badgeW = 0, m_badgeH = 0;
    CBox                          m_badgeBox; // output px, empty: not drawn
    float                         m_badgeScale = 0;
    uint64_t                      m_badgeSerial = 0;
    // emote files from the config and hyprctl
    CEmoteLoader                  m_emoteLoader;
    wl_event_source*              m_emoteSource = nullptr;
    std::vector<std::string>      m_emoteFiles;             // added with hyprctl, absolute
    std::vector<std::string>      m_emoteLoading;           // being built
    std::string                   m_emotesConfigured = "\n"; // "\n" = none seen
    std::string                   m_emotePlay;              // plays its first emote once built
    int                           m_emotePlayLoop = -1;
    float                         m_bodyYaw = 0;
    V3                            m_wish;             // horizontal velocity the keys ask for
    bool                          m_bodyTurning = false;
    float                         m_lookYaw = 0, m_lookPitch = 0; // head turn relative to the body
    // light around the avatar, measured as the map bakes its own
    struct {
        std::array<float, 32> sky{}, bounce{};
        int                   next = 0;
        bool                  full = false;
        float                 skyAvg = 1, bounceAvg = 0;
    } m_avatarLight;

    // first person body: camera in the avatar's eyes, head hidden
    bool            m_fpBodyOn         = true;
    int             m_fpBodyConfigured = -1;
    SFirstPersonEye m_fpEye;
    bool            m_fpPress = false; // finger pokes next frame
    float           m_fpTouch = 0;     // seconds the finger stays out
    int             m_fpTap   = -1;    // hand tapping next frame (0 left, 1 right)
    bool            m_fpTapRight = false; // space bar: thumbs alternate
    float           m_aimDist = 0;
    // emotes move the whole body: first person steps out behind it (0 eyes .. 1 out)
    float           m_emoteView = 0;
    eFirstHands     m_fpHands   = FPH_READY;
    bool            m_fpCramped = false;     // low ceiling: camera in the body, not drawn

    // third person: the camera hangs behind the avatar's shoulder
    bool  m_thirdPerson = false;
    float m_camDist     = 2.6f; // wanted boom length (wheel)
    float m_camSide     = 0.4f; // over the right shoulder
    float m_camBoom     = 0;    // actual, after walls pulled it in

    // auto exposure: a few rays a frame sample the light
    float                 m_exposure = 1;
    float                 m_lightAvg = 0;
    std::array<float, EXPOSURE_SAMPLES> m_lightProbe{};
    int                   m_lightNext = 0;
    bool                  m_lightFull = false; // false: resample all at once

    // player
    SWalker m_body;
    float m_yaw = 0, m_pitch = 0;
    float m_eyeHeight = EYE;
    bool  m_crouched  = false;
    bool  m_fly       = false;
    bool  m_running   = false;   // Shift held while moving
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
    PHLWINDOWREF                 m_typingInto;          // with its dialogs; typing ends when it goes
    float                        m_typingUnfocused = 0; // seconds without a focused window
    Vector2D                     m_look;
    Vector2D                     m_lastAbs{-1, -1};
    struct {
        bool     negative = false;
        uint32_t axis = 0, timeMs = 0;
        uint32_t acc = 0; // unsent 1/120ths of a notch
    } m_wheel;            // hi-res wheel summed into notches
    bool                         m_axisFramePending = false; // touchpad frame still to send
    // away: input on another monitor's desktop, the 3D view staying up
    bool                         m_away = false;
    bool                         m_awayTold = false;   // the way back was shown
    std::optional<Vector2D>      m_desktopAt;          // last cursor spot elsewhere (Super+Esc)
    bool                         m_ownMove = false;    // our own input.mouse.move (a drag)
    UP<SEventLoopDoLaterLock>    m_followLater;        // followCursor() after this frame

    // a layer surface with the keyboard (a launcher), drawn over the 3D view
    struct {
        PHLLSREF               layer;
        bool                   input   = false; // gets keys and mouse
        float                  closing = 0;     // seconds since it let go of the keyboard
        Vector2D               pointer;         // over the monitor, logical px
        WP<CWLSurfaceResource> grab;            // has the pointer until release
        Vector2D               grabAt;          // global logical px
    } m_shell;

    // the Action Menu (Tab), like VRChat's: the mouse moves its cursor while open
    CActionMenu m_menu{[this](const std::string& id) {
                           if (id == "apps" || id == "apps/all" || id == "windows" || id.starts_with("win:") || id.starts_with("close:") || id == "maps" ||
                               id == "avatars")
                               return ownPage(id);
                           const auto gain = m_lip.gainSetting();
                           return actionPage(id, {m_avatar.get(), &m_anim, m_avatarLoader.busy(), m_thirdPerson, m_fly, m_lipsync, CMicrophone::available(),
                                                  gain ? *gain : NAN, m_lip.gain(), m_mapLoader.busy() ? "loading…" : m_world.name});
                       },
                       [this](const SMenuItem& it, float v, float v2) { // slider or stick dial
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
    // pointer on the panels for the app's cursor (panel key 0 = none)
    struct {
        uintptr_t panel = 0;
        Vector2D  local;
    } m_pointerAt;
    bool                                   m_cursorShown = false;
    std::vector<PHLMONITORREF>             m_cursorLocks; // hardware cursor off while in 3D
    std::unordered_set<const CWLSurfaceResource*> m_drawnSurfaces; // the panels' surfaces this frame
    Time::steady_tp                        m_frameTime;

    // play mode (P): a window (a game) gets all input, relative motion while it locks the pointer
    struct {
        bool         on = false;
        bool         fill = false; // camera faces it (Shift+P, play_view fill)
        PHLWINDOWREF window;
        Vector2D     pointer;    // window-local logical px
        Vector2D     box;        // last drawn size; the pointer stays on it
        Vector2D     lastGlobal; // Hyprland's cursor; in 3D only warps move it
        float        t = 0;      // 0 own view .. 1 facing it (fill)
        bool         framed = false; // eye and rot face it
        uintptr_t    framedKey = 0;  // ... for this window
        std::string  configured = "\n";
        float        unfocused    = 0;     // seconds without a focused window
        V3           eye;
        Quat         rot;
        // view share played here in the ring (play_size, Super+wheel); centre: turn the row to it
        float                                 size   = PLAY_SIZE;
        bool                                  centre = false;
        std::optional<uint32_t>               sizeConfigured; // as bits, so NaN compares equal
        float                                 wheel = 0;      // fraction of a notch
        std::string                           sizeTell;       // Super+wheel result, told after a pause
        std::chrono::steady_clock::time_point sizeAt;         // ... that notch
        bool                                  wallTold = false; // wall hint told this play
    } m_play;
    // autoPlay: declined = fullscreen before 3D or play ended in fullscreen; fresh = newly fullscreen, unchecked
    struct {
        std::vector<PHLWINDOWREF> declined, fresh;
    } m_fullscreen;

    SCamera                   m_camera;
    SCamera                   m_ownCamera; // own view this frame, ignoring fill play
    V3                        m_camFwd{0, 0, -1}, m_camUp{0, 1, 0};
    M4                        m_view, m_proj;
    float                     m_depthMul = 0;

    // apps: desktop entries (re-read after 30 s), launches, rules, window spots by class
    std::vector<SAppEntry>                       m_apps;
    std::chrono::steady_clock::time_point        m_appsRead{};
    // Maps and Avatars pages' files, re-read every 2 s while shown
    struct SModelFile {
        std::string path;  // absolute
        std::string label; // its name, or its folder's
        uintmax_t   bytes = 0;
    };
    std::vector<SModelFile>                      m_mapFiles, m_avatarFiles;
    std::chrono::steady_clock::time_point        m_mapFilesRead{}, m_avatarFilesRead{};
    struct SLaunch {
        std::string                           token; // HYPRWALK_LAUNCH, marks its windows
        std::string                           what, cls; // expected class, "" = any
        std::string                           steam;     // Steam app id: only its window is ours
        int64_t                               pid = 0;
        std::chrono::steady_clock::time_point at;
    };
    std::vector<SLaunch>                         m_launches;
    uint64_t                                     m_launchCount = 0;
    // Steam windows opened in 3D: say how to play unless played soon (once a minute per class)
    struct SPlayHint {
        std::string                           cls;
        PHLWINDOWREF                          window;
        std::chrono::steady_clock::time_point at;
    };
    std::vector<SPlayHint>                                                 m_playHints;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_playHinted;
    // shortcut hold: until you turn or move from here, or a moment passes
    struct {
        bool                                  on = false;
        float                                 yaw = 0, pitch = 0;
        V3                                    feet;
        std::chrono::steady_clock::time_point at;
    } m_shortcutHold;
    PHLWINDOWREF                          m_shortcutFor; // focused window at the last shortcut
    std::chrono::steady_clock::time_point m_shortcutAt;  // ... when
    // game window a shortcut unfocused during the hold, refocused after (updateHolds)
    PHLWINDOWREF m_giveBack;
    // play ended by itself: the game's keys are held back till Super+Esc, P, its refocus or PLAY_PAUSE_IDLE
    struct {
        bool                                  on = false;
        std::chrono::steady_clock::time_point at; // last held-back input (or the start)
        PHLWINDOWREF                          window;
        int64_t                               pid = 0;
        std::string                           cls, title;
        // the game's other windows as its focused one closed; Hyprland focusing one (focus_on_close) won't resume play
        std::vector<PHLWINDOWREF> others;
    } m_held;
    std::string                                  m_rulesConfigured = "\n";
    std::vector<SAppRule>                        m_rules;
    std::unordered_map<std::string, SWindowSpot> m_spots;
    std::string                                  m_spotsFor = "\n"; // map they're for, "" = the courtyard
    uint64_t                                     m_pinCount = 0;
    size_t                                       m_iconNext = 0; // next icon to load
    WP<CWLSurfaceResource>                       m_dndAt;   // last drag target and point
    Vector2D                                     m_dndLocal;

    // windows out in the world, by window
    std::unordered_map<uintptr_t, SPlacement> m_placements;
    struct {
        uintptr_t  key = 0;      // window being carried, 0 = none
        float      dist = 2;     // distance past carryFrom(); Ctrl+wheel
        float      scaleMul = 1; // 1 = wall size; wheel
        bool       hadBefore = false;
        SPlacement before;       // to put it back on escape
        int        tileAt = -1;    // row slot it left, -1 = none
        bool       onWall = false; // flat on a surface
    } m_hold;

    // tiling mode (T): the ring, the row, and what undoes it
    struct {
        bool                                      on          = false;
        bool                                      follow      = true;  // goes with you (Y)
        bool                                      anchorLater = false; // re-anchor next frame in 3D
        STileRing                                 ring;
        std::vector<uintptr_t>                    order;  // the row, left to right
        std::unordered_map<uintptr_t, SPlacement> before; // world placement before (none: wall)
        std::unordered_set<uintptr_t>             stay;   // stay put after tiling
        std::unordered_set<uintptr_t>             walled; // sent to the wall: not re-gathered
        std::unordered_set<uintptr_t>             kept;   // put down off the ring: out of the row
        struct SLaid {
            float angle = 0, scale = 0, w = 0, h = 0, pull = 1;
            V3    at;             // ring centre then
            bool  played = false; // played here: not pulled in
        };
        std::unordered_map<uintptr_t, SLaid>      laid;           // layout and obstacle pull-in
        STileFloor                                floor;          // floor under a following ring
        V3                                        moved;          // ... its move this frame
        int                                       holdSlot         = -1; // carried window's slot, -1 = none
        int                                       configured       = -1;
        int                                       configuredFollow = -1;
        // the window played here as last laid out; frozen while its keys are held back
        struct {
            bool                   on = false;
            float                  dy = 0, dist = 0, pitch = 0, angle = 0;
            std::vector<uintptr_t> order;
        } look;
    } m_tiling;

    bool                      m_prevDSBlocked = false;
    UP<SEventLoopDoLaterLock> m_restoreLater;

    // Hyprland glue
    std::vector<CHyprSignalListener> m_listeners;
    wl_event_source*                 m_configTimer = nullptr; // config poll
    SP<SHyprCtlCommand>              m_ctlCommand;
    CFunctionHook*                   m_hookMoved  = nullptr;
    CFunctionHook*                   m_hookWarp   = nullptr;
    CFunctionHook*                   m_hookCursor = nullptr;
    CFunctionHook*                   m_hookWheel  = nullptr;
    CFunctionHook*                   m_hookFrame  = nullptr;
    CFunctionHook*                   m_hookSoftCursor = nullptr;
    CFunctionHook*                   m_hookDiscard    = nullptr;
    // aquamarine idle events holding removed headless outputs (holdOutput)
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
    std::string                      handsStatus() const;
    std::string                      wristsStatus() const;
    std::string                      loadEmoteFile(const std::string& file, int loop);
    std::string                      setLipSync(bool on);
    void                             lipSync(); // every frame: mic to mouth
    void                             emoteSound();
    std::string                      emoteSoundStatus() const;
    void                             watchMicrophone(); // mic problems for the badge
    std::string                      lipSyncStatus() const;
    std::string                      setLipSyncGain(const std::string& v); // dB, or auto
    void                             setLipSyncSource(const std::string& v); // "" = default
    std::vector<std::string>         emoteFiles() const; // the config's, then those added
    void                             loadEmoteFiles(std::vector<std::string> files);
    void                             applyEmotes(SEmoteResult&& res);
    std::string                      menuAction(const SMenuItem& item);
    SMenuPage                        ownPage(const std::string& id); // apps, apps/all, windows, win:ADDRESS, maps, avatars
    const std::vector<SAppEntry>&    apps();
    static void                      listModelFiles(std::vector<SModelFile>& out, const char* folder, std::initializer_list<std::string_view> exts,
                                                    std::initializer_list<std::string> also);
    const std::vector<SModelFile>&   mapFiles();
    std::string                      pickMap(const std::string& path);
    const std::vector<SModelFile>&   avatarFiles();
    std::string                      pickAvatar(const std::string& path);
    std::string                      launch(const std::string& what);
    void                             onWindowOpen(const PHLWINDOW& w);
    void                             onFullscreen(const PHLWINDOW& w);
    void                             autoPlay();
    bool                             fullscreenHere(const PHLWINDOW& w) const;
    bool                             focusable(const PHLWINDOW& w) const;
    bool                             placeInFront(const PHLWINDOW& w, const SAppRule& rule);
    // metres per logical px `dist` m ahead: `height` m tall, or 0 = screen size within `fit` of the view
    float                            frontScale(const Vector2D& size, float dist, float height, float fit = FRONT_FIT) const;
    // apparent size from your eye, 1 = as on the 2D desktop
    float                            apparentSize(const V3& center, float scale) const;
    // how far (0.1..1) a window must come toward the eye, scaled with it, to be unobstructed
    float                            clearance(const V3& eye, const V3& c, const V3& r, const V3& u, const V3& n, float hw, float hh) const;
    float                            carryFrom() const;
    void                             placeAt(const PHLWINDOW& w, const SWindowSpot& spot);
    bool                             inSight(const SWindowSpot& spot) const;
    void                             loadSpots();
    void                             rememberSpot(uintptr_t key);
    void                             forgetSpot(uintptr_t key);
    void                             restoreSpots();
    PHLWINDOW                        findWindow(const std::string& what) const; // an address (0x...), a class or a title
    std::string                      windowAction(const PHLWINDOW& w, eWindowAction a);
    std::string                      setPinned(const PHLWINDOW& w, bool on);
    std::string                      togglePin();
    std::string                      carry();
    std::string                      resizeReal(const PHLWINDOW& w, const Vector2D& size);
    std::string                      windowsStatus() const;
    void                             anchorRing(bool look = true);
    void                             followRing(float dt);
    void                             fitRing();
    float                            ringHeight() const;
    bool                             atRing() const;
    void                             keepFromRow(uintptr_t key);
    void                             gatherTiles();
    void                             tileWindow(const PHLWINDOW& w, int slot);
    int                              lookSlot() const;    // where in the row you look
    float                            ringLookYaw() const;
    float                            ringLookYaw(const V3& eye, float yaw) const;
    std::vector<STileIn>             tileSizes(const std::vector<uintptr_t>& keys) const;
    PHLWINDOW                        playedInRow() const;
    void                             updateTiling(float dt);
    void                             forgetTiles();  // placements dropped: row restarts
    bool                             tileable(const PHLWINDOW& w) const;
    void                             checkTilingConfig();
    std::string                      tilingStatus() const;
    void                             checkAppRules();
    void                             menuPick(const std::optional<SMenuItem>& item);
    std::string                      setView(bool third);
    bool                             fpBody() const;
    void                             animateAvatar(float dt);
    void                             measureAvatarLight();
    M4                               avatarTransform() const;
    void                             dropPlacements();
    std::string                      placeDesktop(float height);
    SCamera                          flatCamera() const;
    SCamera                          playerCamera() const;
    V3                               camPivot() const;   // (third person, with an avatar)
    V3                               avatarHead() const; // (the same)
    SCamera                          viewCamera(float dt);
    void                             update();
    void                             simulate(float dt);
    bool                             overlaps(const V3& feet, float height) const;
    void                             layoutPanels(float e);
    void                             updatePlacements(float dt);
    void                             grab();
    void                             takePinned(uintptr_t key);
    void                             releaseButtons();
    void                             place();
    void                             cancelHold();
    void                             returnToWall(uintptr_t key);
    SPlacement                       layoutPlacement(const SPanel& p) const;
    void                             aim();
    void                             updatePointer(uint32_t timeMs = 0, bool frame = true, bool relative = false);
    void                             pointerFrame(bool relative = false);
    int                              windowPanel(const PHLWINDOW& w) const; // its panel's index, -1 = none
    void                             endPlay();
    void                             playEnded(const char* why);
    void                             declineFullscreen(const PHLWINDOW& played);
    bool                             playedWith(const SPanel& q, const PHLWINDOW& w) const;
    void                             updatePlay(float dt);
    void                             sizePlayed(int notches);
    float                            playedShare() const;
    void                             updateTyping(float dt);
    bool                             keyboardWith(const PHLWINDOW& w) const;
    void                             aimPlay();
    void                             clampPlayPointer();
    void                             playMotion(const IPointer::SMotionEvent& e);
    std::string                      playStatus() const;
    void                             lockCursors(bool lock);
    bool                             onMouseMove(const Vector2D& pos);
    bool                             cursorAway() const;
    void                             followCursor();
    void                             goAway(bool refocus);
    void                             comeBack();
    std::optional<Vector2D>          desktopSpot() const;
    void                             addAppCursor();
    PHLLS                            keyboardLayer() const;
    void                             updateShell(float dt);
    void                             shellPointer(uint32_t timeMs = 0);
    void                             shellMotion(const IPointer::SMotionEvent& e);
    void                             drawShell(const PHLMONITOR& mon);
    void                             exitNow();
    void                             restore();
    void                             resetPlayer();
    void                             adaptExposure(float dt);
    void                             onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info);
    void                             focusForShortcut();
    void                             onWindowClose(const PHLWINDOW& w);
    bool                             sameGame(const PHLWINDOW& w) const;
    bool                             playable(const PHLWINDOW& w) const;
    std::string                      playAgain(const PHLWINDOW& w, bool fill);
    std::string                      playHeldGame(bool fill);
    void                             updateHolds();
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
    template <typename... Flags> // hypr::SOFT_CURSOR_FLAGS
    using FnSoftCursor = void (*)(void*, PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D>, Flags...);
    using FnDiscard    = void (*)(CSurfacePassElement*);

    CFunctionHook* g_moved  = nullptr;
    CFunctionHook* g_warp   = nullptr;
    CFunctionHook* g_cursor = nullptr;
    CFunctionHook* g_wheel  = nullptr;
    CFunctionHook* g_frame  = nullptr;
    CFunctionHook* g_softCursor = nullptr;
    CFunctionHook* g_discard    = nullptr;
    // device of the wheel event being handled (input.mouse.axis doesn't carry it)
    WP<IPointer> g_wheelPointer;

    void           hkMouseMoved(void* self, IPointer::SMotionEvent e) {
        if (g_p3D && g_p3D->onRelativeMotion(e))
            return;
        ((FnMouseMoved)g_moved->m_original)(self, e);
    }

    // in 3D the plugin draws the app's cursor on its panel: skip Hyprland's software cursor, but keep sending the
    // cursor surface its frame callbacks
    template <typename... Flags>
    void hkSoftCursors(void* self, PHLMONITOR mon, const Time::steady_tp& now, CRegion& damage, std::optional<Vector2D> at, Flags... flags) {
        if (g_p3D && g_p3D->holdsPointer()) {
            if (const auto surf = hypr::pointer()->currentCursorImage().surface.lock(); surf && surf->resource())
                surf->resource()->frame(now);
            return;
        }
        ((FnSoftCursor<Flags...>)g_softCursor->m_original)(self, mon, now, damage, at, flags...);
    }

    // surfaces we draw get presented feedback after the frame, not Hyprland's "discarded"
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

    // in 3D only cursor:invisible hides the cursor; Hyprland's timeout / key press hiding doesn't apply
    void hkEnsureCursor(void* self) {
        if (g_p3D && g_p3D->holdsPointer()) {
            static auto PINVISIBLE = CConfigValue<Config::INTEGER>("cursor:invisible");
            // without hkSoftCursors it stays hidden (the app's cursor too)
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
    m_body.overlaps = [this](const V3& feet, float height) { return overlaps(feet, height); };
    m_hookMoved  = g_moved  = hookByName("onMouseMoved", "CInputManager::onMouseMoved(", (void*)&hkMouseMoved);
    m_hookWarp   = g_warp   = hookByName("onMouseWarp", "CInputManager::onMouseWarp(", (void*)&hkMouseWarp);
    m_hookCursor = g_cursor = hookByName("ensureCursorRenderingMode", "HyprRenderer::ensureCursorRenderingMode(", (void*)&hkEnsureCursor);
    m_hookWheel  = g_wheel  = hookByName("onMouseWheel", "CInputManager::onMouseWheel(", (void*)&hkMouseWheel);
    m_hookFrame  = g_frame  = hookByName("onPointerFrame", "CInputManager::onPointerFrame(", (void*)&hkPointerFrame);
    m_hookSoftCursor = g_softCursor = hookByName("renderSoftwareCursorsFor", "CPointerManager::renderSoftwareCursorsFor(",
                                                  hypr::SOFT_CURSOR_FLAGS == 2 ? (void*)&hkSoftCursors<bool, bool> : (void*)&hkSoftCursors<bool>);
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
        // redraw everything without damageMonitor(), which would schedule an unpaced extra frame
        mon->m_damage.damageEntire();
    }));

    m_listeners.emplace_back(ev.render.stage.listen([this](eRenderStage stage) {
        if (stage == RENDER_POST) {
            // presented feedback for what we drew, at the monitor's pace (hkDiscard held back "discarded")
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
            return; // exit done: show the real desktop

        g_pHyprRenderer->m_renderPass.add(makeUnique<C3DElement>());
        drawShell(mon);
        // notifications and the error bar went under the 3D view: draw them again on top
        if (mon == Desktop::focusState()->monitor()) {
            Notification::overlay()->draw(mon);
            ErrorOverlay::overlay()->draw();
        }
        // the cursor of the layer over the view
        static auto PINVISIBLE = CConfigValue<Config::INTEGER>("cursor:invisible");
        if (m_shell.input && !*PINVISIBLE && g_pSeatManager->m_state.pointerFocus)
            if (const auto tex = hypr::pointer()->getCurrentCursorTexture(); tex) {
                const Vector2D size = hypr::pointer()->cursorSizeLogical();
                CTexPassElement::SRenderData cursor;
                cursor.tex = tex;
                cursor.box = CBox{(m_shell.pointer - hypr::pointer()->hotspot()) * mon->m_scale, size * mon->m_scale};
                cursor.a   = 1.f;
                g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(cursor)));
            }
        // keep frames coming at the display's pace (scheduleFrameForMonitor would add an unpaced one)
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

    m_listeners.emplace_back(ev.window.open.listen([this](const PHLWINDOW& w) { onWindowOpen(w); }));
    m_listeners.emplace_back(ev.window.fullscreen.listen([this](const PHLWINDOW& w) { onFullscreen(w); }));
    m_listeners.emplace_back(ev.window.close.listen([this](const PHLWINDOW& w) { onWindowClose(w); }));

    m_listeners.emplace_back(ev.input.mouse.button.listen([this](const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        onButton(e.timeMs, e.button, e.state == WL_POINTER_BUTTON_STATE_PRESSED, &info);
    }));

    m_listeners.emplace_back(ev.input.mouse.axis.listen([this](const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) { onAxis(e, info); }));

    m_listeners.emplace_back(ev.input.mouse.move.listen([this](const Vector2D& pos, Event::SCallbackInfo& info) {
        if (onMouseMove(pos))
            info.cancelled = true;
    }));

    // X11 pointer focus in 3D: a zero relative motion frame at once, before a game reads the axes (pointerFrame)
    m_listeners.emplace_back(g_pSeatManager->m_events.pointerFocusChange.listen([this] {
        if (m_mode != MODE_OFF && !m_away && fromXWayland(g_pSeatManager->m_state.pointerFocus.lock()))
            pointerFrame();
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
        checkTilingConfig();
        checkPlayConfig();
    }));
    // run-time values (hyprctl keyword, hl.config()) come without a reload: poll every second
    m_configTimer = wl_event_loop_add_timer(
        g_pCompositor->m_wlEventLoop,
        [](void* data) {
            auto* self = (CDesktop3D*)data;
            self->checkMapConfig();
            self->checkAvatarConfig();
            self->checkAppRules();
            self->checkTilingConfig();
            self->checkPlayConfig();
            wl_event_source_timer_update(self->m_configTimer, 1000);
            return 0;
        },
        this);
    wl_event_source_timer_update(m_configTimer, 1000);

    m_ctlCommand = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{
                                                                    .name  = "hyprwalk",
                                                                    .exact = false,
                                                                    .fn    = [this](eHyprCtlOutputFormat, std::string request) { return hyprctl(request); },
                                                                });

    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwalk:toggle", [this](std::string) {
        toggle();
        return SDispatchResult{};
    });

    // hyprwalk:play [on|off|toggle] [here|fill]
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwalk:play", [this](std::string arg) {
        const std::string r = playDispatch(arg);
        return r.starts_with("error: ") ? SDispatchResult{.success = false, .error = r.substr(7)} : SDispatchResult{};
    });

    // hyprwalk:menu [page|command]
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwalk:menu", [this](std::string arg) {
        const std::string r = menuDispatch(arg);
        return r.starts_with("error: ") ? SDispatchResult{.success = false, .error = r.substr(7)} : SDispatchResult{};
    });

    // hyprwalk:away toggles away (Super+Esc)
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwalk:away", [this](std::string) {
        const std::string r = setAway(!m_away);
        return r.starts_with("error: ") ? SDispatchResult{.success = false, .error = r.substr(7)} : SDispatchResult{};
    });

    // hyprwalk:tile [here|follow]: T, Shift+T, Y
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwalk:tile", [this](std::string arg) {
        const std::string r = tileDispatch(arg);
        return r.starts_with("error: ") ? SDispatchResult{.success = false, .error = r.substr(7)} : SDispatchResult{};
    });

    checkMapConfig();
    checkAvatarConfig();
    checkAppRules();
    checkTilingConfig();
    checkPlayConfig();
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
    // Hyprland clears the render pass only at the next frame; a leftover element's destructor would run after unload
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
    // their idle events run plugin code: dequeue them before unloading
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
    HyprlandAPI::removeDispatcher(PHANDLE, "hyprwalk:toggle");
    HyprlandAPI::removeDispatcher(PHANDLE, "hyprwalk:menu");
    HyprlandAPI::removeDispatcher(PHANDLE, "hyprwalk:play");
    HyprlandAPI::removeDispatcher(PHANDLE, "hyprwalk:tile");

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
        // a loaded map depends only on the desktop's width, not the monitor
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
    forgetTiles();

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
    forgetTiles();
    if (any)
        hypr::updateSuspendedStates();
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
        return; // nothing set: the courtyard is there
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
        return; // superseded

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
        buildWorldFor(mon->m_size); // only anchors the desktop
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
    if (const int body = !g_cfgFirstPersonBody || g_cfgFirstPersonBody->value(); body != m_fpBodyConfigured) {
        m_fpBodyConfigured = body;
        m_fpBodyOn         = body;
    }
    if (const int lip = g_cfgLipSync && g_cfgLipSync->value(); lip != m_lipsyncConfigured) {
        const bool first    = m_lipsyncConfigured < 0;
        m_lipsyncConfigured = lip;
        if (!first || lip)
            setLipSync(lip);
    }
    // hyprctl's value holds until the config's changes
    if (const std::string gain = g_cfgLipSyncGain ? g_cfgLipSyncGain->value() : "auto"; gain != m_lipsyncGainConfigured) {
        m_lipsyncGainConfigured = gain;
        if (const std::string r = setLipSyncGain(gain); r.starts_with("error"))
            notify(r + " (plugin:hyprwalk:lipsync_gain)", true);
    }
    if (const std::string source = g_cfgLipSyncSource ? g_cfgLipSyncSource->value() : ""; source != m_lipsyncSourceConfigured) {
        m_lipsyncSourceConfigured = source;
        setLipSyncSource(source);
    }

    // changed emotes reload the avatar
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
    // emotes in progress were for the old avatar; the new one loads them
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
        return; // superseded

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
    m_ctl.loaded(); // reapply hand-set state
    m_bodyYaw          = m_yaw;
    m_lookYaw          = m_lookPitch = 0;
    m_avatarLight.full = false;
    m_fpEye.reset();
    if (first) {
        m_thirdPerson = true;
        m_camBoom     = m_camDist;
    }
    notify(std::format("avatar {} loaded ({} triangles, {:.2f} m{}), V switches the view", m_avatar->name, m_avatar->triangles, m_avatar->height,
                       m_avatar->humanoid ? "" : ", not a humanoid"));
    // emote files added since the request
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

// builds emotes in the background, with those in progress
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
        return; // avatar changed
    m_emoteLoading.clear();
    const bool play = !m_emotePlay.empty() && std::ranges::find(res.req.files, m_emotePlay) != res.req.files.end();
    if (res.emotes.empty()) {
        // don't retry with the next avatar
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
        // a folder: its first emote
        if (first < 0)
            first = m_anim.findEmote(res.emotes.front()->name);
        m_anim.playEmote(first, m_emotePlayLoop);
        m_emotePlay.clear();
    }
    notify(std::format("emotes: {}{}", names, res.error.empty() ? "" : " (" + res.error + ")"));
}

// first person: hand mode, arm weight and wrists in the view (0..1 from the top left, null behind)
std::string CDesktop3D::handsStatus() const {
    if (!fpBody() || !m_avatar)
        return "null";
    static constexpr const char* MODES[] = {"ready", "touch", "type", "hold", "down"};
    const auto& g  = m_anim.globals();
    const M4    vp = m_proj * m_view, toWorld = avatarTransform() * m_avatar->fix;
    std::string at;
    for (int b : {HB_L_HAND, HB_R_HAND}) {
        const int n = m_avatar->human[b];
        if (n < 0 || (size_t)n >= g.size()) {
            at += at.empty() ? "null" : ", null";
            continue;
        }
        const V3    p = toWorld.point({g[n].m[12], g[n].m[13], g[n].m[14]});
        const float x = vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8] * p.z + vp.m[12], y = vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9] * p.z + vp.m[13],
                    w = vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15];
        at += (at.empty() ? "" : ", ") + (w > 1e-4f ? std::format("[{:.3f}, {:.3f}]", 0.5f + 0.5f * x / w, 0.5f - 0.5f * y / w) : std::string("null"));
    }
    return std::format(R"({{"mode": "{}", "arms": {:.2f}, "at": [{}]}})", MODES[m_fpHands], m_anim.firstPersonArms(), at);
}

// wrists from the feet in the avatar's frame (x right, y up, -z ahead, m); null if not humanoid
std::string CDesktop3D::wristsStatus() const {
    const auto& g = m_anim.globals();
    std::string at;
    for (int b : {HB_L_HAND, HB_R_HAND}) {
        const int n = m_avatar->human[b];
        if (!m_avatar->humanoid || n < 0 || (size_t)n >= g.size())
            return "null";
        const V3 p = m_avatar->fix.point({g[n].m[12], g[n].m[13], g[n].m[14]}) + V3{0, m_anim.lift(), 0};
        at += std::format("{}[{:.3f}, {:.3f}, {:.3f}]", at.empty() ? "" : ", ", p.x, p.y, p.z);
    }
    return "[" + at + "]";
}

std::string CDesktop3D::avatarStatus() const {
    if (!m_avatar)
        return std::format(R"({{"path": "{}", "loading": {}, "view": "first"}})", jsonEscape(m_avatarPath), m_avatarLoader.busy());
    const auto& a = *m_avatar;
    static constexpr const char* EYES[] = {"still", "bones", "expressions"};
    const int                    held   = m_anim.expression();
    const int emote = m_anim.emote();
    return std::format(R"({{"path": "{}", "loading": {}, "name": "{}", "triangles": {}, "joints": {}, "height": {:.3f}, "humanoid": {}, "rig": "{}", "clips": {}, "playing": "{}", "view": "{}", "body": {}, "eyeHeight": {:.3f}, "firstPerson": {:.2f}, "emoteView": {:.2f}, "hands": {}, "wrists": {}, "distance": {:.2f}, "bodyYaw": {:.1f}, "light": [{:.2f}, {:.2f}], "expressions": {}, "expressionsFrom": "{}", "expression": "{}", "gestures": ["{}", "{}"], "eyes": "{}", "parts": {}, "toggles": {}, "sliders": {}, "variants": {}, "settings": "{}", "physics": {}, "springs": {}, "springBones": {}, "springsFrom": "{}", "constraints": {}, "emote": "{}", "emotes": {}, "attack": {}, "gait": {}}})",
                       jsonEscape(m_avatarPath), m_avatarLoader.busy(), jsonEscape(a.name), a.triangles, a.joints.size(), a.height, a.humanoid, jsonEscape(a.humanFrom),
                       a.clips.size(), jsonEscape(m_anim.playing()), m_thirdPerson ? "third" : "first", fpBody(), a.eyeHeight, m_anim.firstPerson(), m_emoteView, handsStatus(), wristsStatus(), m_camBoom, m_bodyYaw * 180.f / F_PI, m_avatarLight.skyAvg,
                       m_avatarLight.bounceAvg, a.expressions.size(), jsonEscape(a.expressionsFrom), held >= 0 ? jsonEscape(a.expressions[held].name) : "",
                       gestureName(m_anim.gesture(0)), gestureName(m_anim.gesture(1)), EYES[a.lookAt.type], a.parts.size(), a.toggles.size(), a.sliders.size(), a.variants.size(), jsonEscape(a.settings),
                       m_anim.physics(), a.springs.size(), a.springJoints.size(), jsonEscape(a.springsFrom), a.constraints.size(),
                       emote >= 0 ? jsonEscape(m_anim.emotes()[emote]->name) : "", m_anim.emotes().size(), m_anim.attackStatus(), m_anim.gaitStatus());
}

// hyprctl hyprwalk avatar emote FILE|FOLDER: build, then play the first emote
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
        return m_avatarLoader.busy() ? "error: the avatar is still loading" : "error: no avatar loaded (set plugin:hyprwalk:avatar or use hyprctl hyprwalk avatar <file>)";
    if (third && !m_thirdPerson)
        m_camBoom = 0.3f; // pull out from the head
    m_thirdPerson = third;
    m_play.centre = m_play.on && !m_play.fill; // re-centre a ring-played window
    return third ? "third" : "first";
}

// first_person_body, for an avatar with eyes
bool CDesktop3D::fpBody() const {
    return m_fpBodyOn && m_avatar && m_avatar->eyeHeight > 0.f && !m_thirdPerson;
}

M4 CDesktop3D::avatarTransform() const {
    return M4::trs(m_body.seen() + V3{0, m_anim.lift(), 0}, Quat::axisAngle({0, 1, 0}, -m_bodyYaw), {1, 1, 1});
}

// sky and bounce light on the avatar like the map's baked values, a few rays a frame
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
    const V3    from  = m_body.seen() + V3{0, std::max(0.3f, m_avatar->height * 0.55f), 0};
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

// the body turns to where it walks (CAvatarAnimator::wayToFace), the head to where the camera looks
void CDesktop3D::animateAvatar(float dt) {
    if (!m_avatar)
        return;

    const V3    hv    = m_mode == MODE_ACTIVE ? V3{m_body.vel.x, 0, m_body.vel.z} : V3{};
    const float speed = length(hv);
    SAvatarMotion mo;
    mo.dt       = dt;
    mo.speed    = speed;
    mo.vel      = hv;
    mo.wish     = m_mode == MODE_ACTIVE ? V3{m_wish.x, 0, m_wish.z} : V3{};
    mo.accel    = ACCEL;
    mo.decel    = DECEL;
    mo.turnBack = TURN_BACK;
    mo.vy       = m_body.vel.y;
    mo.onGround = m_body.onGround || m_mode != MODE_ACTIVE;
    mo.flying   = m_fly;
    mo.crouched = m_crouched;
    mo.run      = m_running && m_mode == MODE_ACTIVE;

    float want = m_bodyYaw;
    // third person: face where the keys go; reversing, it turns as it slows
    if (const auto way = m_anim.wayToFace(mo, m_thirdPerson, m_bodyYaw, m_yaw)) {
        want          = *way;
        m_bodyTurning = false;
    } else if (speed > 0.3f) {
        const float to = std::atan2(hv.x, -hv.z), side = wrapAngle(to - m_yaw);
        want           = m_thirdPerson ? to : std::abs(side) > 2.2f ? m_yaw : m_yaw + std::clamp(side, -0.8f, 0.8f);
        m_bodyTurning  = false;
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
    m_bodyYaw = m_anim.turnBody(m_bodyYaw, want, dt, speed);

    float lookYaw = wrapAngle(m_camera.yaw - m_bodyYaw), lookPitch = m_camera.pitch;
    if (m_thirdPerson && std::abs(lookYaw) > 1.75f) {
        // the camera looks at its face: look back at it
        const V3 d = m_camera.eye - (m_body.seen() + V3{0, m_avatar->height * 0.92f, 0});
        lookYaw    = wrapAngle(std::atan2(d.x, -d.z) - m_bodyYaw);
        lookPitch  = std::atan2(d.y, std::hypot(d.x, d.z));
    }
    // turning far: look where the body turns
    if (m_thirdPerson) {
        const float turning = smoothstep01((std::abs(m_anim.turnLeft()) - 0.3f) / 0.6f);
        lookYaw *= 1.f - turning, lookPitch *= 1.f - turning;
    }
    // first person body: the head follows the camera at once
    const float k = fpBody() ? 1.f : 1.f - std::exp(-dt * 6.f);
    m_lookYaw += (std::clamp(lookYaw, -1.4f, 1.4f) - m_lookYaw) * k;
    m_lookPitch += (std::clamp(lookPitch, -1.1f, 1.1f) - m_lookPitch) * k;

    mo.lookYaw   = m_lookYaw;
    mo.lookPitch = m_lookPitch;
    // first person hands do what you do, within reach of walls and the aimed window (eyes' camera, not the drawn one)
    const SCamera eyes = playerCamera();
    mo.fp.on    = fpBody();
    mo.fp.eye   = eyes.eye;
    mo.fp.yaw   = eyes.yaw;
    mo.fp.pitch = eyes.pitch;
    if (mo.fp.on) {
        // hands down while playing, or while an emote's camera is out
        mo.fp.hands = m_play.on || m_play.t > 0.f || m_emoteView > 0.f ? FPH_DOWN
            : m_hold.key                                             ? FPH_HOLD
            : m_typing                            ? FPH_TYPE
            : (!m_sentButtons.empty() && m_drag.surface && !m_shell.input) || m_fpTouch > 0.f ? FPH_TOUCH
                                                                                              : FPH_READY;
        m_fpHands   = mo.fp.hands;
        mo.fp.press = m_fpPress && mo.fp.hands == FPH_TOUCH;
        mo.fp.tap   = mo.fp.hands == FPH_TYPE ? m_fpTap : -1;
        mo.fp.room  = 1e9f;
        if (SRayHit hit; m_world.collision.raycast(eyes.eye, forwardFrom(eyes.yaw, eyes.pitch), 1.5f, hit))
            mo.fp.room = hit.t;
        if (m_aimed >= 0 && m_aimed < (int)m_panels.size() && !m_panels[m_aimed].front)
            mo.fp.room = std::min(mo.fp.room, m_aimDist);
        if (m_hold.key) // carried window, from the eye
            mo.fp.room = std::min(mo.fp.room, m_hold.dist);
    }
    m_fpTouch = m_fpPress ? 0.25f : std::max(0.f, m_fpTouch - dt);
    m_fpPress = false;
    m_fpTap   = -1;
    mo.world     = M4::trs(m_body.seen(), Quat::axisAngle({0, 1, 0}, -m_bodyYaw), {1, 1, 1}); // (up stairs smoothly)
    // ground a little above or below the feet (stairs, slopes)
    mo.ground = [this](float x, float z, float y) { return groundUnder(m_world.collision, x, z, y); };
    emoteSound();
    m_anim.update(mo);

    measureAvatarLight();
}

// plays an emote's sound in 3D, the dance following the audio clock; out of 3D it stops at once
void CDesktop3D::emoteSound() {
    const auto&   all    = m_anim.emotes();
    const int     e      = m_mode != MODE_OFF ? m_anim.emote() : -1;
    const auto*   em     = e >= 0 && e < (int)all.size() ? all[e].get() : nullptr;
    const float   volume = g_cfgEmoteVolume ? std::clamp((float)g_cfgEmoteVolume->value(), 0.f, 1.f) : EMOTE_VOLUME;
    const SSound* sound  = em && volume > 0 ? em->sound.get() : nullptr;
    if (e != m_soundFor.emote || m_anim.emoteStarts() != m_soundFor.starts || sound != m_soundFor.sound) {
        m_soundFor = {e, m_anim.emoteStarts(), sound};
        if (!sound) {
            if (m_mode == MODE_OFF)
                m_speaker.stopNow();
            else
                m_speaker.stop(0.3f); // (as the emote fades out)
        } else if (std::string error; !m_speaker.play(em->sound, m_anim.emoteLooping(), volume, em->name, error, m_anim.emoteTime() / em->speed)) {
            if (error != m_soundError)
                notify(std::format("{}'s sound: {}", em->name, error), true);
            m_soundError = error;
        } else
            m_soundError.clear();
    }
    m_speaker.update();
    if (!sound || !m_speaker.on()) {
        // report a stream error once
        if (const SSpeakerStatus st = sound ? m_speaker.status() : SSpeakerStatus{}; !st.error.empty() && st.error != m_soundError) {
            notify(std::format("{}'s sound: {}", em->name, st.error), true);
            m_soundError = st.error;
        }
        return;
    }
    m_speaker.setVolume(volume);
    if (const double t = m_speaker.clock(); t >= 0)
        m_anim.setEmoteClock(t);
}

std::string CDesktop3D::emoteSoundStatus() const {
    const SSpeakerStatus s = m_speaker.status();
    return std::format(R"({{"available": {}, "volume": {:.2f}, "on": {}, "emote": "{}", "file": "{}", "stream": "{}", "error": "{}", "at": {:.3f}, "position": {:.3f}, )"
                       R"("start": {:.3f}, "duration": {:.3f}, "loop": {}, "latency": {:.4f}}})",
                       CSpeaker::available(), g_cfgEmoteVolume ? (float)g_cfgEmoteVolume->value() : EMOTE_VOLUME, s.on, jsonEscape(s.name), jsonEscape(s.file), s.stream,
                       jsonEscape(s.error), s.at, s.position, s.start, s.duration, s.loop, s.latency);
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
    d.center = eye + dir * hit.t + n * 0.01f;
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

    if (const std::string want = configuredMonitor(); !mon && !want.empty()) {
        mon = monitorNamed(want);
        if (!mon)
            notify(std::format("plugin:hyprwalk:monitor: no monitor {} is connected, so 3D goes on the focused one", want), true);
    }
    if (!mon)
        mon = Desktop::focusState()->monitor();
    if (!mon)
        mon = hypr::monitorAtCursor();
    if (!mon || mon->m_size.x < 1 || mon->m_size.y < 1)
        return false;

    buildWorldFor(mon->m_size);

    if (m_restoreLater)
        m_restoreLater.reset(); // still leaving 3D: state saved already
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
    // windows already fullscreen aren't autoplayed
    m_fullscreen.declined.clear();
    m_fullscreen.fresh.clear();
    for (const auto& w : hypr::windows())
        if (w && w->m_isMapped && hypr::fullscreen(w))
            m_fullscreen.declined.emplace_back(w);

    // bring the cursor and focus to the 3D monitor, so new windows open there
    if (const Vector2D cursor = hypr::pointer()->position(); hypr::monitorAt(cursor) != mon) {
        m_desktopAt = cursor;
        hypr::warpCursor(mon->middle(), true);
    }
    Desktop::focusState()->rawMonitorFocus(mon);
    unfocusOthers(mon);

    g_pHyprRenderer->m_directScanoutBlocked = true;
    lockCursors(true);
    g_pHyprRenderer->ensureCursorRenderingMode(); // hkEnsureCursor
    g_pHyprRenderer->damageMonitor(mon);
    hypr::scheduleFrame(mon);
    restoreSpots(); // they fly from the wall
    if (m_tiling.on)
        m_tiling.anchorLater = true; // ring round where you enter

    log("entering 3D on " + mon->m_name);
    return true;
}

// hardware cursor off on every monitor in 3D (hkSoftCursors keeps the software one off)
void CDesktop3D::lockCursors(bool lock) {
    if (!lock) {
        for (const auto& m : m_cursorLocks)
            if (const auto mon = m.lock())
                hypr::pointer()->unlockSoftwareForMonitor(mon);
        m_cursorLocks.clear();
        return;
    }
    for (const auto& mon : hypr::monitors()) {
        if (!mon || std::ranges::any_of(m_cursorLocks, [&](const auto& m) { return m.lock() == mon; }))
            continue;
        hypr::pointer()->lockSoftwareForMonitor(mon);
        m_cursorLocks.emplace_back(mon);
    }
    // clear the edge resize arrow (general:hover_icon_on_border): 3D never removes it, and it blocks apps' cursors
    hypr::Cursor::overrideController->unsetOverride(hypr::Cursor::CURSOR_OVERRIDE_WINDOW_EDGE);
}

// ------------------------------------------------------------ other monitors

// true keeps the move from Hyprland; a keybind moving the focus to another monitor goes away
bool CDesktop3D::onMouseMove(const Vector2D& pos) {
    if (m_mode == MODE_OFF || m_ownMove)
        return false;
    const auto mon = m_monitor.lock();
    const auto at  = hypr::monitorAt(pos);
    if (m_away) {
        if (at != mon) {
            m_desktopAt = pos;
            return false;
        }
        if (m_mode != MODE_ACTIVE)
            return false; // leaving 3D anyway
        comeBack();
        return true;
    }
    if (mon && at && at != mon && m_mode == MODE_ACTIVE && !m_play.on && at == Desktop::focusState()->monitor()) {
        goAway(false); // the move goes on, focusing what's under it
        m_desktopAt = pos;
        return false;
    }
    return true;
}

// on another monitor: stay away, or go once it has the focus (not in play mode)
bool CDesktop3D::cursorAway() const {
    const auto mon = m_monitor.lock();
    const auto at  = hypr::monitorAtCursor();
    if (!mon || !at || at == mon)
        return false;
    return m_away || (!m_play.on && at == Desktop::focusState()->monitor());
}

// for cursor moves Hyprland doesn't report (a focus keybind warping, a monitor removed)
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
    // release buttons on their window before the pointer leaves it
    if (!m_sentButtons.empty()) {
        const uint32_t t = nowMs();
        for (uint32_t b : m_sentButtons)
            g_pSeatManager->sendPointerButton(t, b, WL_POINTER_BUTTON_STATE_RELEASED);
        pointerFrame();
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
    // release a game's pointer lock, as Hyprland's keybinds do
    g_pInputManager->unconstrainMouse();
    lockCursors(false);
    g_pHyprRenderer->ensureCursorRenderingMode();
    if (refocus) {
        g_pInputManager->simulateMouseMovement(); // focus what's under the cursor
        pointerFrame(); // Hyprland's move has no frame
    }
    const auto at = hypr::monitorAtCursor();
    log(std::format("the mouse and keyboard went to {}, 3D stays on {}", at ? at->m_name : "?", mon->m_name));
    if (!m_awayTold) {
        m_awayTold = true;
        notify(std::format("the mouse is on {} now; move it back onto {}, or press Super+Esc, to walk in 3D again", at ? at->m_name : "your desktop", mon->m_name));
    }
}

// the cursor came back onto the 3D monitor
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
    if (unfocusOthers(mon))
        m_fullscreen.fresh.clear(); // 3D took the keyboard: autoPlay focuses none
    log("the mouse and keyboard came back into 3D on " + mon->m_name);
}

// Super+Esc spot: the last one on another monitor, else the nearest monitor's middle
std::optional<Vector2D> CDesktop3D::desktopSpot() const {
    const auto mon = m_monitor.lock();
    if (!mon)
        return std::nullopt;
    PHLMONITOR nearest;
    for (const auto& m : hypr::monitors()) {
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

// Super+Esc, hyprctl hyprwalk away, hyprwalk:away
std::string CDesktop3D::setAway(bool away) {
    const auto mon = m_monitor.lock();
    if (m_mode != MODE_ACTIVE || !mon)
        return "error: not in 3D";
    if (!away) {
        if (m_away) {
            if (hypr::monitorAtCursor() != mon)
                hypr::warpCursor(mon->middle(), true); // (the focus too)
            comeBack();
        }
        return "in 3D";
    }
    if (m_away)
        return "away";
    const auto to = desktopSpot();
    if (!to)
        return "error: there's no other monitor for the mouse to go to";
    hypr::warpCursor(*to, true); // (the focus too)
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
        place(); // what's carried stays where it is

    if (const auto mon = m_monitor.lock())
        hypr::scheduleFrame(mon);
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
    m_shell  = {};
    m_drawnSurfaces.clear();
    m_followLater.reset();
    lipSync();    // closes the mic (no update() out of 3D)
    emoteSound(); // stops an emote's sound
    if (!m_restoreLater)
        m_restoreLater = g_pEventLoopManager->doLaterLock([this] {
            m_restoreLater.reset(); // safe: the queue already moved this callback out
            restore();
        });
    if (const auto mon = m_monitor.lock())
        g_pHyprRenderer->damageMonitor(mon);
}

// aquamarine < 0.12.1 runs a headless output's late frame as an idle event holding `this` (fixed in 1699271, 6ecde03):
// a use-after-free in CBackend::dispatchIdle if the output goes first. Our idle event, queued after, keeps it alive
void CDesktop3D::holdOutput(const PHLMONITOR& mon) {
    const auto backend = g_pCompositor->m_aqBackend;
    if (!mon || !mon->m_output || !backend)
        return;
    if (const auto impl = mon->m_output->getBackend(); !impl || impl->type() != Aquamarine::AQ_BACKEND_HEADLESS)
        return;
    // drop holds whose event has run (only we still hold them)
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
        pointerFrame();
    m_sentButtons.clear();
    m_drag   = {};
    m_typing = false;
    m_away   = false;
    endPlay();
    m_play.t = 0;
    m_keys.fill(false);
    if (m_hold.key)
        place();
    // let Hyprland suspend placed windows on hidden workspaces again
    if (!m_placements.empty())
        hypr::updateSuspendedStates();
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
    pointerFrame(); // (as going away does)

    if (const auto mon = m_monitor.lock())
        g_pHyprRenderer->damageMonitor(mon);

    log("left 3D");
}

// E: keys to the aimed (else focused) window till Super+Esc, or it leaves the view or loses the keyboard
std::string CDesktop3D::setTyping(bool on) {
    if (m_mode != MODE_ACTIVE)
        return on ? "error: not in 3D" : "walking";
    if (m_away)
        return on ? "error: the mouse and keyboard are on another monitor" : "walking";
    if (!on) {
        if (m_play.on || m_held.on)
            return setPlay(false); // also ends play and held keys
        if (m_typing)
            log("typing off");
        m_typing = false;
        m_typingInto.reset();
        m_keys.fill(false);
        return "walking";
    }
    if (m_play.on)
        return "playing";
    PHLWINDOW w;
    if (m_aimed >= 0 && m_aimed < (int)m_panels.size() && m_panels[m_aimed].kind != PANEL_LAYER)
        w = m_panels[m_aimed].window.lock();
    else if (const auto focus = Desktop::focusState()->window(); focus && windowPanel(focus) >= 0)
        w = focus;
    if (!w)
        return "error: point the crosshair at a window to type into it";
    auto into = w; // the dialog's top window
    for (auto up = parentOf(w); up && windowPanel(up) >= 0; up = parentOf(up))
        into = up;
    m_typing          = true;
    m_typingInto      = into;
    m_typingUnfocused = 0;
    m_shortcutHold.on = false; // (a window chosen)
    m_held.on         = false;
    m_keys.fill(false);
    m_menu.hide();
    if (w != Desktop::focusState()->window())
        Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK);
    notify(std::format("typing into {}: Super+Esc to walk again", w->m_title.empty() ? w->m_class : w->m_title));
    return "typing";
}

void CDesktop3D::resetPlayer() {
    m_body.feet     = m_world.spawn;
    m_body.vel      = {};
    m_body.seenY    = NAN; // (seen there at once)
    m_yaw           = m_world.spawnYaw;
    m_pitch         = 0;
    m_eyeHeight     = EYE;
    m_body.onGround = true;
    m_crouched  = false;
    m_fly       = false;
    m_lightFull = false;
    m_bodyYaw     = m_yaw;
    m_bodyTurning = false;
    m_camBoom     = m_camDist;
    m_avatarLight.full = false;
    m_fpEye.reset();
}

// auto exposure adapting like eyes, within the map's range if set (CS2 post processing volume); never windows
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
    if (fpBody() && m_fpEye.live) // avatar's eyes, as last drawn
        return {m_fpEye.at(m_body.seen(), m_yaw), m_yaw, m_pitch};
    return {m_body.seen() + V3{0, m_eyeHeight, 0}, m_yaw, m_pitch}; // smooth on stairs
}

// boom pivot: the avatar's head, over its shoulder unless in a wall (camPivot)
V3 CDesktop3D::avatarHead() const {
    const float crouch = m_eyeHeight / EYE;
    return m_body.seen() + V3{0, std::max(0.5f, m_avatar->height * 0.95f * crouch + 0.15f), 0};
}

V3 CDesktop3D::camPivot() const {
    const V3 right{std::cos(m_yaw), 0, std::sin(m_yaw)};
    const V3 head = avatarHead();
    float    side = m_camSide;
    SRayHit     hit;
    if (side > 0 && m_world.collision.raycast(head, right, side + 0.25f, hit))
        side = std::max(0.f, hit.t - 0.25f);
    return head + right * side;
}

// third person: behind the avatar's shoulder, pulled in by walls
SCamera CDesktop3D::viewCamera(float dt) {
    // first person body: camera at the eyes, held through breathing and bob, in the body's box and under the ceiling
    m_fpCramped = false;
    if (const auto eyes = fpBody() ? m_anim.eyes() : std::nullopt) {
        const V3 feet = m_body.seen(), at = avatarTransform().point(*eyes);
        float    high = 1e9f;
        if (SRayHit hit; m_world.collision.raycast({at.x, feet.y + 0.3f, at.z}, {0, 1, 0}, 3.f, hit))
            high = 0.3f + hit.t - 0.08f;
        m_fpEye.reach = RADIUS - 0.04f, m_fpEye.low = 0.3f, m_fpEye.high = high;
        m_fpEye.update(feet, at, m_yaw, dt);
        m_fpCramped = at.y - feet.y > high + 0.05f;
    } else
        m_fpEye.reset();
    SCamera c = playerCamera();
    // first person emote: ease out behind the head and back
    const bool  emote = fpBody() && m_mode == MODE_ACTIVE && m_anim.emote() >= 0;
    const float was   = m_emoteView;
    m_emoteView       = std::clamp(m_emoteView + (emote ? dt : -dt) / 0.45f, 0.f, 1.f);
    if (m_emoteView > 0.f && was <= 0.f)
        m_camBoom = 0.3f;
    if ((!m_thirdPerson && m_emoteView <= 0.f) || !m_avatar)
        return c;

    const V3 fwd = forwardFrom(m_yaw, m_pitch);
    const V3 right{std::cos(m_yaw), 0, std::sin(m_yaw)};
    const V3 up    = cross(right, fwd);
    const V3 pivot = camPivot();

    // a few rays around the boom, a thick one so the near plane stays out of walls too
    constexpr float R    = 0.18f;
    float           want = m_camDist;
    SRayHit         hit;
    for (const V3& o : {V3{}, right * R, right * -R, up * R, up * -R}) {
        if (m_world.collision.raycast(pivot + o, -fwd, want + 0.2f, hit))
            want = std::max(0.f, hit.t - 0.2f);
    }
    // in at once, out gently
    m_camBoom = want < m_camBoom ? want : m_camBoom + (want - m_camBoom) * (1.f - std::exp(-dt * 3.f));
    c.eye     = m_thirdPerson ? pivot - fwd * m_camBoom : lerp(c.eye, pivot - fwd * m_camBoom, smoothstep01(m_emoteView));
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
    updateHolds();

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
    m_ownCamera        = view;
    V3            fwd, up{0, 1, 0};
    if (m_mode == MODE_ACTIVE) {
        m_camera = view;
        fwd      = forwardFrom(m_camera.yaw, m_camera.pitch);
        // fill play: the camera moves to face the played window
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
    // Apps and Windows page icons, a few a frame
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
    m_panels = collectPanels(mon, layerSpacing(), placed, e);
    // drawn windows Hyprland doesn't render (under a fullscreen one) get no frame callbacks and would freeze
    for (const auto& p : m_panels)
        if (const auto w = p.kind == PANEL_WINDOW && !m_placements.contains(p.key) ? p.window.lock() : nullptr;
            w && w->m_isMapped && !g_pHyprRenderer->shouldRenderWindow(w, mon) && w->wlSurface() && w->wlSurface()->resource())
            w->wlSurface()->resource()->breadthfirst([&now](SP<CWLSurfaceResource> s, const Vector2D&, void*) { s->frame(now); }, nullptr);
    updateShell(dt);
    updatePlacements(dt);
    layoutPanels(e);
    autoPlay(); // after poses: here play turns you to it
    if (m_play.on || m_play.t > 0.f)
        updatePlay(dt);
    if (m_typing && !m_play.on)
        updateTyping(dt);

    m_drawnSurfaces.clear();
    for (const auto& p : m_panels)
        for (const auto& s : p.surfaces)
            if (const auto res = s.surface.lock())
                m_drawnSurfaces.insert(res.get());
    if (const auto ls = m_shell.layer.lock(); ls && ls->m_mapped && ls->wlSurface() && ls->wlSurface()->resource()) {
        // (drawn over the view: presented, not discarded under it)
        ls->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> s, const Vector2D&, void*) { m_drawnSurfaces.insert(s.get()); }, nullptr);
        if (ls->m_popupHead)
            ls->m_popupHead->breadthfirst(
                [this](SP<Desktop::View::CPopup> p, void*) {
                    if (p && p->wlSurface() && p->wlSurface()->resource())
                        m_drawnSurfaces.insert(p->wlSurface()->resource().get());
                },
                nullptr);
    }

    if (m_mode == MODE_ACTIVE && !m_away && m_shell.input) {
        shellPointer();
        m_aimed       = -1;
        m_pointerAt   = {};
        m_cursorShown = false;
    } else if (m_mode == MODE_ACTIVE && !m_away) {
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

    // cursor changed monitors without a reported move: follow after this frame
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
    updateTiling(dt);
    // a following ring moves its windows and dialogs at once; only row changes ease
    if (const V3 d = m_tiling.moved; length(d) > 0.f)
        for (auto& [key, pl] : m_placements) {
            const auto up = pl.follows ? m_placements.find(pl.follows) : m_placements.end();
            if (pl.tiled || (up != m_placements.end() && up->second.tiled && !pl.returning && !pl.pinned && key != m_hold.key))
                pl.center += d;
        }
    // a wall dialog whose window is in the world follows it, in front; layoutPlacement as panel poses come later
    for (const auto& p : m_panels) {
        if (p.kind != PANEL_WINDOW || m_placements.contains(p.key))
            continue;
        const auto w      = p.window.lock();
        const auto parent = w && w->m_isMapped ? parentOf(w) : nullptr;
        const auto pp     = parent ? m_placements.find(reinterpret_cast<uintptr_t>(parent.get())) : m_placements.end();
        if (pp == m_placements.end() || pp->second.returning || pp->second.pinned)
            continue;
        const uintptr_t follows = pp->first;
        SPlacement&     pl      = m_placements[p.key];
        pl                      = layoutPlacement(p);
        pl.window               = w;
        pl.follows              = follows;
    }
    if (m_placements.empty())
        return;

    const auto mon = m_monitor.lock();
    const auto now = Time::steadyNow();
    const V3   eye = m_camera.eye;
    const V3   dir = forwardFrom(m_camera.yaw, m_camera.pitch);

    // pinned windows stack down the view's right side, pin_size of its height each, 0.8 m ahead
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
            // move with the view at once; fill-played: with your own view, not the moving camera
            const bool  own   = m_play.t > 0.f && m_play.framed && p.key == m_play.framedKey;
            const V3    oFwd  = forwardFrom(m_ownCamera.yaw, m_ownCamera.pitch);
            const V3    vEye  = own ? m_ownCamera.eye : eye;
            const V3    vFwd  = own ? oFwd : m_camFwd;
            const V3    vUp   = own ? cross(V3{std::cos(m_ownCamera.yaw), 0, std::sin(m_ownCamera.yaw)}, oFwd) : m_camUp;
            const float scale = pinScale(p), w = (float)p.box.w * scale, h = (float)p.box.h * scale;
            const V3    right = normalize(cross(vFwd, vUp));
            pl.targetCenter   = vEye + vFwd * pinD + right * (pinHalfW - pinMargin - w * 0.5f) + vUp * (pinHalfH - pinMargin - pinAbove[p.key] - h * 0.5f);
            pl.targetRot      = Quat::fromBasis(right, vUp, -vFwd);
            pl.targetScale    = scale;
            pl.center         = pl.targetCenter;
            pl.rot            = pl.targetRot;
            pl.scale          = pl.targetScale;
        } else if (p.key == m_hold.key) {
            const float scale = m_screen.scale() * m_hold.scaleMul;
            const V3    camRight{std::cos(m_camera.yaw), 0, std::sin(m_camera.yaw)};
            const V3    camUp = cross(camRight, dir);
            // third person: carried past the avatar
            const float from  = carryFrom();
            const V3    start = eye + dir * from;

            V3      c, r, u, n;
            float   pull = 1.f;
            SRayHit hit;
            m_hold.onWall = m_world.collision.raycast(start, dir, m_hold.dist, hit);
            if (m_hold.onWall) {
                // flat on what it touches; on floors and ceilings turned to read from where you stand
                n = hit.normal;
                const V3 upHint = std::abs(n.y) > 0.85f ? camUp : V3{0, 1, 0};
                u               = normalize(upHint - n * dot(upHint, n));
                if (length(u) < 0.5f)
                    u = perpendicular(n);
                r = normalize(cross(u, n));
                c = start + dir * hit.t + n * 0.02f;

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
                c = start + dir * m_hold.dist;

                // pull it in (same size on screen) so it doesn't sink into things; at least 0.4 m past the avatar
                pull = clearance(eye, c, r, u, n, (float)p.box.w * 0.5f * scale, (float)p.box.h * 0.5f * scale);
                if (from > 0.f)
                    pull = std::max(pull, std::min(1.f, (from + 0.4f) / (from + m_hold.dist)));
            }

            pl.targetCenter = eye + (c - eye) * pull;
            pl.targetRot    = Quat::fromBasis(r, u, n);
            pl.targetScale  = scale * pull;
        }

        // hidden-workspace windows are suspended without frame callbacks and would freeze
        if (mon) {
            const auto w = pl.window.lock();
            if (w && !g_pHyprRenderer->shouldRenderWindow(w, mon)) {
                w->setSuspended(false);
                if (w->wlSurface() && w->wlSurface()->resource())
                    w->wlSurface()->resource()->breadthfirst([&now](SP<CWLSurfaceResource> s, const Vector2D&, void*) { s->frame(now); }, nullptr);
            }
        }
    }

    // dialogs stay in front of their window as on the 2D desktop (not a pinned one's)
    for (auto& [key, pl] : m_placements) {
        if (!pl.follows || pl.returning || pl.pinned || key == m_hold.key)
            continue;
        const auto pit    = m_placements.find(pl.follows);
        const auto w      = pl.window.lock();
        const auto parent = pit != m_placements.end() ? pit->second.window.lock() : nullptr;
        if (!w || !parent || pit->second.returning) {
            pl.returning = true;
            continue;
        }
        const SPlacement& o = pit->second;
        if (o.pinned)
            continue;
        const Vector2D at = (hypr::realPosition(w)->goal() + hypr::realSize(w)->goal() * 0.5) - (hypr::realPosition(parent)->goal() + hypr::realSize(parent)->goal() * 0.5);
        pl.targetCenter   = o.targetCenter + o.targetRot.rotate({(float)at.x * o.targetScale, -(float)at.y * o.targetScale, 0.06f});
        pl.targetRot      = o.targetRot;
        pl.targetScale    = o.targetScale;
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
            it = m_placements.erase(it); // off screen: it's home anyway
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
    // near the flat desktop everything stacks as in 2D, placed windows too
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
        p.front      = pl.pinned != 0 && inWorld; // pinned: over the world, like the HUD
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
    // out of the tiling row while carried (escape restores it)
    m_hold.tileAt = -1;
    if (const auto o = std::ranges::find(m_tiling.order, key); o != m_tiling.order.end()) {
        m_hold.tileAt = (int)(o - m_tiling.order.begin());
        m_tiling.order.erase(o);
    }

    // pick it up where it's drawn now, unpinned and unparented
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
    pl.tiled        = false;
    pl.follows      = 0;

    m_hold.key      = key;
    m_hold.dist     = std::clamp(t - carryFrom(), 0.8f, 6.f); // third person: past the avatar
    m_hold.scaleMul = pl.scale / m_screen.scale();
    releaseButtons();
}

// a pinned window back into your hands at its size and distance before pinning; Esc or a right click re-pins
void CDesktop3D::takePinned(uintptr_t key) {
    const auto it = m_placements.find(key);
    const auto w  = it != m_placements.end() ? it->second.window.lock() : nullptr;
    if (!w || !it->second.pinned)
        return;
    SPlacement& pl   = it->second;
    m_hold.hadBefore = true;
    m_hold.before    = pl;
    m_hold.tileAt    = -1;
    pl.pinned        = 0; // flies from the corner to where you point
    m_hold.key       = key;
    m_hold.dist      = std::clamp(pl.pinnedFromDist > 0 ? pl.pinnedFromDist : 2.f, 0.8f, 6.f);
    const float scale = pl.pinnedFromScale > 0 ? pl.pinnedFromScale : frontScale(hypr::realSize(w)->goal(), carryFrom() + m_hold.dist, 0, carryFrom() > 0 ? THIRD_FIT : FRONT_FIT);
    m_hold.scaleMul  = std::clamp(scale / m_screen.scale(), 0.2f, 5.f);
    releaseButtons();
}

// let go of anything the client thinks is pressed
void CDesktop3D::releaseButtons() {
    const uint32_t now = nowMs();
    for (uint32_t b : m_sentButtons)
        g_pSeatManager->sendPointerButton(now, b, WL_POINTER_BUTTON_STATE_RELEASED);
    if (!m_sentButtons.empty())
        pointerFrame();
    m_sentButtons.clear();
    m_drag = {};
    m_shell.grab.reset();
}

void CDesktop3D::place() {
    // slides into the spot last aimed at (remembered per class); in tiling mode, put down in the air it joins the row
    const uintptr_t key    = m_hold.key;
    const bool      onWall = m_hold.onWall;
    const int       slot   = m_tiling.holdSlot;
    m_hold                 = {};
    m_tiling.holdSlot      = -1;
    if (m_tiling.on) {
        const auto it = m_placements.find(key);
        const auto w  = it != m_placements.end() ? it->second.window.lock() : nullptr;
        if (w && !onWall && tileable(w) && atRing()) {
            // no slot yet: by its place round the ring
            const auto& r  = m_tiling.ring;
            const int   at = slot >= 0 ? slot : ringSlot(r, layoutRing(r, tileSizes(m_tiling.order)), ringYaw(r, it->second.center));
            it->second.tiled = true;
            m_tiling.order.insert(m_tiling.order.begin() + std::min<size_t>(at, m_tiling.order.size()), key);
            m_tiling.kept.erase(key);
            m_tiling.walled.erase(key);
            return;
        }
        if (w) // stays when tiling ends
            keepFromRow(key);
    }
    rememberSpot(key);
}

void CDesktop3D::cancelHold() {
    const uintptr_t  key    = m_hold.key;
    const bool       had    = m_hold.hadBefore;
    const SPlacement before = m_hold.before;
    const int        tileAt = m_hold.tileAt;
    m_hold                  = {};
    m_tiling.holdSlot       = -1;
    if (!had) {
        returnToWall(key);
        return;
    }
    if (auto it = m_placements.find(key); it != m_placements.end()) {
        it->second.targetCenter = before.targetCenter;
        it->second.targetRot    = before.targetRot;
        it->second.targetScale  = before.targetScale;
        it->second.pinned       = before.pinned;  // re-pinned
        it->second.follows      = before.follows; // follows its window again
        if (tileAt >= 0 && m_tiling.on) {         // back into its row slot
            it->second.tiled = true;
            m_tiling.order.insert(m_tiling.order.begin() + std::min<size_t>(tileAt, m_tiling.order.size()), key);
        }
    }
}

void CDesktop3D::returnToWall(uintptr_t key) {
    if (m_hold.key == key)
        m_hold = {};
    if (auto it = m_placements.find(key); it != m_placements.end())
        it->second.returning = true;
    if (m_tiling.on) // kept out of the tiling row
        m_tiling.walled.insert(key);
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
        m_body.vel = lerp(m_body.vel, v, std::min(1.f, dt * 12.f));
        m_wish     = {};
        m_crouched = false;
        m_running  = false;
    } else {
        // crouching, standing up only when there is room
        if (crouchKey)
            m_crouched = true;
        else if (m_crouched && !overlaps(m_body.feet, HEIGHT))
            m_crouched = false;

        const float walk  = g_cfgWalkSpeed ? g_cfgWalkSpeed->value() : WALK_SPEED, run = g_cfgRunSpeed ? g_cfgRunSpeed->value() : RUN_SPEED;
        const float speed = m_crouched ? std::min(CROUCH_SPEED, walk) : sprint ? std::max(run, walk) : walk;
        const V3    target = wish * speed;
        m_wish             = target;
        V3          horiz{m_body.vel.x, 0, m_body.vel.z};
        const float accel = m_body.onGround ? (dot(target, horiz) < 0 ? TURN_BACK : length(target) < length(horiz) ? DECEL : ACCEL) : 10.f;
        m_running         = sprint && !m_crouched && length(wish) > 0.5f;
        V3          diff   = target - horiz;
        const float dl     = length(diff);
        const float maxD   = accel * dt;
        if (dl > maxD)
            diff = diff * (maxD / dl);
        m_body.vel.x += diff.x;
        m_body.vel.z += diff.z;

        m_body.vel.y -= GRAVITY * dt;
        m_body.vel.y = std::max(m_body.vel.y, -40.f);
        if (jump && m_body.onGround) {
            m_body.vel.y    = JUMP_SPEED;
            m_body.onGround = false;
        }
    }

    m_body.move(dt, m_crouched ? HEIGHT_CROUCH : HEIGHT, m_fly);

    // fell out somehow
    const float floorY = m_world.model ? m_world.bounds.min.y : 0.f;
    if (m_body.feet.y < floorY - 10.f || !std::isfinite(m_body.feet.x + m_body.feet.y + m_body.feet.z))
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
        // aim through where it takes no input, as on the 2D desktop (full-screen shell overlays)
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

    // hidden by the world unless pinned
    if (SRayHit hit; !m_panels[m_aimed].front && m_world.collision.raycast(eye, dir, best - 1e-3f, hit)) {
        m_aimed = -1;
        return;
    }

    const auto& p   = m_panels[m_aimed];
    m_aimPanelLocal = bestLocal;
    m_aimDist       = best;
    if (p.hitRoot) {
        const auto [surf, local] = p.hitRoot->at(m_aimPanelLocal * p.hitScale - p.hitOffset, true);
        if (surf) {
            m_aimSurface = surf;
            m_aimLocal   = local;
        }
    }
}

// XWayland gives X clients a frame's move (maybe a stale one from 2D) from its absolute device unless the frame has
// relative motion; SDL3 keeps the axes of the first move it sees, so mouse-look then got only the change between moves,
// nothing for a steady turn. Every frame to XWayland thus carries a relative motion, zero unless `relative`
void CDesktop3D::pointerFrame(bool relative) {
    if (!relative && fromXWayland(g_pSeatManager->m_state.pointerFocus.lock()))
        PROTO::relativePointer->sendRelativeMotion((uint64_t)nowMs() * 1000, {}, {});
    g_pSeatManager->sendPointerFrame();
}

// `relative`: the mouse's own relative motion already went out this frame (play mode)
void CDesktop3D::updatePointer(uint32_t timeMs, bool frame, bool relative) {
    SP<CWLSurfaceResource> surf;
    Vector2D               local;
    m_pointerAt = {};

    if (m_drag.surface && !(PROTO::data && PROTO::data->dndActive())) {
        // implicit grab: keep sending to the pressed surface, relative to its panel plane
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

    // a drag and drop follows the crosshair: pointer focus and Hyprland's move event (otherwise held back)
    if (PROTO::data && PROTO::data->dndActive()) {
        g_pSeatManager->setPointerFocus(surf, local);
        if (surf && (surf != m_dndAt.lock() || local != m_dndLocal)) {
            m_dndAt    = surf;
            m_dndLocal = local;
            const auto hl = Desktop::View::CWLSurface::fromResource(surf);
            if (const auto box = hl ? hl->getSurfaceBoxGlobal() : std::nullopt) {
                Event::SCallbackInfo info;
                m_ownMove = true; // don't go away to another monitor
                Event::bus()->m_events.input.mouse.move.emit(box->pos() + local, info);
                m_ownMove = false;
            }
        }
        return;
    }

    const auto current = g_pSeatManager->m_state.pointerFocus.lock();
    if (surf != current) {
        // leaving an X11 window: a frame first, so XWayland's held move goes out relative (pointerFrame)
        if (fromXWayland(current))
            pointerFrame(relative); // play: carries the mouse's own motion
        g_pSeatManager->setPointerFocus(surf, local);
        m_lastSentLocal = local;
        // XWayland: the position as a move in this frame too, replacing a stale one (pointerFrame)
        if (fromXWayland(surf)) {
            const uint32_t t = timeMs ? timeMs : nowMs();
            PROTO::relativePointer->sendRelativeMotion((uint64_t)t * 1000, {}, {});
            g_pSeatManager->sendPointerMotion(t, local);
        }
        if (surf && frame)
            g_pSeatManager->sendPointerFrame();
        return;
    }

    if (surf && local != m_lastSentLocal) {
        const uint32_t t = timeMs ? timeMs : nowMs();
        // XWayland: a move without the mouse's motion gets a zero relative one (pointerFrame)
        if (!relative && fromXWayland(surf))
            PROTO::relativePointer->sendRelativeMotion((uint64_t)t * 1000, {}, {});
        g_pSeatManager->sendPointerMotion(t, local);
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

namespace {
    bool listed(const std::vector<PHLWINDOWREF>& windows, const PHLWINDOW& w) {
        return std::ranges::any_of(windows, [&](const PHLWINDOWREF& r) { return r.lock() == w; });
    }

    // drawn size in panel px: a Wayland window's buffer (frames off its animated box), an X11 window's box
    Vector2D drawnSize(const SPanel& p) {
        const Vector2D s = p.hitRoot ? p.hitRoot->m_current.size / p.hitScale : Vector2D{};
        return s.x >= 1 && s.y >= 1 ? s : p.box.size();
    }
}

std::string CDesktop3D::setPlay(bool on, std::optional<bool> fill) {
    if (!on) {
        if (m_held.on) { // stop holding back a game's keys
            m_held.on = false;
            declineFullscreen(m_held.window.lock()); // ended by you: no autoplay
            notify("walking again");
        }
        if (!m_play.on)
            return "walking";
        declineFullscreen(m_play.window.lock()); // (you ended it)
        endPlay();
        m_typing = false;
        m_keys.fill(false);
        log("play mode off");
        return "walking";
    }
    checkPlayConfig(); // catch a just-set play_size
    if (m_play.on) {
        // view switch while playing: the camera goes to it or back to you, turned to it if out of view
        if (fill && *fill != m_play.fill) {
            m_play.fill     = *fill;
            m_play.centre   = !m_play.fill; // here: turn the row to the view
            const auto w    = m_play.window.lock();
            const int  i    = w ? windowPanel(w) : -1;
            const V3   mid  = i >= 0 ? m_panels[i].pose.at(m_panels[i].box.size() * 0.5) : V3{};
            if (i >= 0 && !m_play.fill && !inOwnView(mid))
                turnTo(mid);
            const auto name = !w ? std::string() : w->m_title.empty() ? w->m_class : w->m_title;
            notify(m_play.fill ? std::format("playing {}, filling the view", name) : std::format("playing {} here", name));
        }
        return "playing";
    }
    if (m_mode != MODE_ACTIVE)
        return "error: not in 3D";
    if (m_away)
        setAway(false); // played while away: back to 3D
    if (m_aimed < 0 || m_aimed >= (int)m_panels.size() || m_panels[m_aimed].kind == PANEL_LAYER)
        return "error: point the crosshair at a window to play it";
    const SPanel& p = m_panels[m_aimed];
    auto          w = p.window.lock();
    // play a dialog's window with it; a fullscreen dialog itself (focusing its window would end fullscreen)
    for (auto up = w ? parentOf(w) : nullptr; up && windowPanel(up) >= 0 && !hypr::fullscreen(w); up = parentOf(up))
        w = up;
    const int own = w ? windowPanel(w) : -1;
    if (own < 0)
        return "error: point the crosshair at a window to play it";

    if (m_hold.key)
        place();
    m_menu.hide();
    m_play.on         = true;
    m_play.fill       = fill.value_or(configuredPlayFill());
    m_play.window     = w;
    m_play.framed     = m_play.framed && m_play.t > 0.f; // continue a camera coming back
    m_play.unfocused  = 0;
    m_play.pointer    = p.box.pos() - m_panels[own].box.pos() + m_aimPanelLocal; // where the crosshair was
    m_play.box        = drawnSize(m_panels[own]);
    m_play.lastGlobal = hypr::pointer()->position();
    m_play.centre     = !m_play.fill;
    m_play.wheel      = 0;
    m_play.sizeTell.clear();
    m_play.wallTold   = false;
    m_shortcutHold.on = false; // (a window chosen)
    // the game, to resume after play ends by itself
    m_held = {.window = w, .pid = w->getPID(), .cls = classOf(w), .title = w->m_title};
    // all keys go to it, and the keyboard focus: SDL reads controllers, and pointer locks work, only with it
    m_typing = true;
    m_keys.fill(false);
    if (!keyboardWith(w)) // its dialog or X11 menu keeps it
        Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK);
    clampPlayPointer();
    // played here out of view (Windows page Play, going fullscreen): turn to face it
    if (const V3 at = p.pose.at(m_aimPanelLocal); !m_play.fill && !inOwnView(at)) {
        turnTo(at);
        logf("turned to face {} (it wasn't in the view)", w->m_class);
    }
    // tell the other view's key, and Super+wheel sizing when placed
    const auto  name   = w->m_title.empty() ? w->m_class : w->m_title;
    const char* other  = m_play.fill == configuredPlayFill() ? "Shift+P" : "P";
    const auto  placed = m_placements.find(reinterpret_cast<uintptr_t>(w.get()));
    const bool  sized  = placed != m_placements.end() && !placed->second.pinned && !placed->second.returning && !placed->second.follows;
    notify(m_play.fill ? std::format("playing {}: Super+Esc gives the mouse and keyboard back ({} plays it in place)", name, other) :
                         std::format("playing {} here: Super+Esc gives the mouse and keyboard back ({} fills the view){}", name, other,
                                     sized ? ". Super+wheel makes it bigger or smaller" : ""));
    return "playing";
}

// hyprwalk:play, hl.plugin.hyprwalk.play(), hyprctl hyprwalk play: [on|off|toggle] [here|fill]; a view alone means on
std::string CDesktop3D::playDispatch(const std::string& arg) {
    std::istringstream in(unquote(arg));
    std::string        v, view;
    for (std::string word; in >> word;) {
        const bool isView = word == "here" || word == "fill", isOn = word == "on" || word == "off" || word == "toggle";
        if ((!isView && !isOn) || !(isView ? view : v).empty()) // at most one of each
            return "error: play [on|off|toggle] [here|fill]";
        (isView ? view : v) = word;
    }
    if (v.empty())
        v = view.empty() ? "toggle" : "on";
    const bool                on   = v == "on" || (v == "toggle" && !m_play.on);
    const std::optional<bool> fill = view.empty() ? std::nullopt : std::optional<bool>(view == "fill");
    if (on && !m_play.on && m_held.on && m_mode == MODE_ACTIVE && !m_away)
        return playHeldGame(fill.value_or(configuredPlayFill()));
    return setPlay(on, fill);
}

// within the player's last drawn view (not play mode's camera)
bool CDesktop3D::inOwnView(const V3& at) const {
    const auto  mon    = m_monitor.lock();
    const float aspect = mon && mon->m_size.y > 0 ? (float)(mon->m_size.x / mon->m_size.y) : 16.f / 9.f;
    const float tanY   = std::tan(FOV_Y * 0.5f);
    const V3    fwd = forwardFrom(m_ownCamera.yaw, m_ownCamera.pitch), right{std::cos(m_ownCamera.yaw), 0, std::sin(m_ownCamera.yaw)};
    const V3    d = at - m_ownCamera.eye;
    const float z = dot(d, fwd);
    return z > 0 && std::abs(dot(d, cross(right, fwd))) <= z * tanY && std::abs(dot(d, right)) <= z * tanY * aspect;
}

// faces a point at once; iterated since the third-person pivot turns with you
void CDesktop3D::turnTo(const V3& at) {
    for (int n = 0; n < 3; ++n) {
        const V3 to = at - (m_thirdPerson && m_avatar ? camPivot() : playerCamera().eye);
        m_yaw       = wrapAngle(std::atan2(to.x, -to.z));
        m_pitch     = std::clamp(std::atan2(to.y, std::hypot(to.x, to.z)), -1.55f, 1.55f);
    }
}

// rereads play_size and play_view; a changed play_size replaces Super+wheel's size
void CDesktop3D::checkPlayConfig() {
    if (const float size = g_cfgPlaySize ? g_cfgPlaySize->value() : PLAY_SIZE; m_play.sizeConfigured != std::bit_cast<uint32_t>(size)) {
        m_play.sizeConfigured = std::bit_cast<uint32_t>(size);
        m_play.size           = std::isnan(size) ? PLAY_SIZE : std::clamp(size, PLAY_SIZE_MIN, PLAY_SIZE_MAX);
        m_play.centre         = m_play.on && !m_play.fill;
        if (m_play.size != size)
            notify(std::format("plugin:hyprwalk:play_size is {} to {}, not {}: {} then", PLAY_SIZE_MIN, PLAY_SIZE_MAX, size, m_play.size), true);
    }
    const std::string v = configuredPlayView();
    if (v == m_play.configured)
        return;
    m_play.configured = v;
    if (!v.empty() && v != "here" && v != "fill")
        notify(std::format("plugin:hyprwalk:play_view is here or fill, not {}: P plays here", v), true);
}

void CDesktop3D::endPlay() {
    m_play.on = false;
    m_play.window.reset();
    m_play.sizeTell.clear();
}

// play ended on its own: hold the game's keys back from hyprwalk (updateHolds)
void CDesktop3D::playEnded(const char* why) {
    if (!m_play.on)
        return;
    if (const auto w = m_play.window.lock(); w && !w->m_title.empty())
        m_held.title = w->m_title;
    endPlay();
    m_typing = false;
    m_keys.fill(false);
    log(std::string("play mode ended: ") + why);
    if (m_mode != MODE_ACTIVE || m_away)
        return;
    m_held.on = true;
    m_held.at = std::chrono::steady_clock::now();
    notify(std::format("play mode ended ({}): the keys are held back. P plays {} again, Super+Esc walks", why, clipped(m_held.title.empty() ? m_held.cls : m_held.title, 40)),
           true);
}

// windows you stopped playing aren't auto-played again while fullscreen
void CDesktop3D::declineFullscreen(const PHLWINDOW& played) {
    if (!played)
        return;
    for (const auto& w : hypr::windows()) {
        if (!w || !w->m_isMapped || !hypr::fullscreen(w) || listed(m_fullscreen.declined, w))
            continue;
        auto o = w;
        for (int hops = 0; o && o != played && hops < 8; ++hops)
            o = parentOf(o);
        if (o == played)
            m_fullscreen.declined.emplace_back(w);
    }
}

// w's own panels, its popups and dialogs, transitively
bool CDesktop3D::playedWith(const SPanel& q, const PHLWINDOW& w) const {
    auto o = q.window.lock();
    for (int hops = 0; o && hops < 8; ++hops, o = parentOf(o))
        if (o == w)
            return true;
    return false;
}

// w has the keyboard, or its dialog or X11 menu does
bool CDesktop3D::keyboardWith(const PHLWINDOW& w) const {
    auto focus = Desktop::focusState()->window();
    if (focus && focus->isX11OverrideRedirect())
        focus = x11Owner(focus);
    for (int hops = 0; focus && focus != w && hops < 8; ++hops)
        focus = parentOf(focus);
    return w && focus == w;
}

// typing (E) ends like play mode, else keys go to whatever Hyprland focuses next
void CDesktop3D::updateTyping(float dt) {
    const auto w = m_typingInto.lock();
    m_typingUnfocused = Desktop::focusState()->window() ? 0.f : m_typingUnfocused + dt;
    const char* why   = !w || windowPanel(w) < 0 ? "the window left the 3D view" :
          keyboardWith(w)                        ? (!g_pInputManager->m_exclusiveLSes.empty() ? "a layer surface took the keyboard" : nullptr) :
          m_typingUnfocused == 0                 ? "another window has the keyboard" :
          m_typingUnfocused >= 0.5f              ? "no window has had the keyboard for half a second" :
                                                   nullptr;
    if (why) {
        log(std::string("typing ended: ") + why);
        m_typing = false;
        m_typingInto.reset();
        m_keys.fill(false);
    }
}

// ends play mode when the window leaves the view or loses the keyboard
void CDesktop3D::updatePlay(float dt) {
    const auto w = m_play.window.lock();
    const int  i = w ? windowPanel(w) : -1;
    if (m_play.on) {
        const bool  ours = keyboardWith(w);
        m_play.unfocused = Desktop::focusState()->window() ? 0.f : m_play.unfocused + dt;
        const char* why  = i < 0 ? "the window left the 3D view" :
              ours                      ? (!g_pInputManager->m_exclusiveLSes.empty() ? "a layer surface took the keyboard" : nullptr) :
              m_play.unfocused == 0     ? "another window has the keyboard" :
              m_play.unfocused >= 0.5f  ? "no window has had the keyboard for half a second" :
                                          nullptr;
        if (why)
            playEnded(why);
        // report Super+wheel's size once the wheel is still
        if (m_play.on && !m_play.sizeTell.empty() && std::chrono::steady_clock::now() - m_play.sizeAt >= std::chrono::duration<float>(PLAY_SIZE_TELL))
            notify(std::exchange(m_play.sizeTell, {}));
    }
    m_play.t = std::clamp(m_play.t + (m_play.on && m_play.fill ? dt : -dt) / PLAY_TIME, 0.f, 1.f);
    if (m_play.t <= 0.f)
        m_play.framed = false;
    if (i < 0)
        return;

    const SPanel& p = m_panels[i];
    if (m_play.fill) {
        const auto  mon    = m_monitor.lock();
        const float aspect = mon && mon->m_size.y > 0 ? (float)(mon->m_size.x / mon->m_size.y) : 16.f / 9.f;
        const float tanY   = std::tan(FOV_Y * 0.5f);
        const float dist   = std::max((float)p.box.h * p.pose.scale * 0.5f / tanY, (float)p.box.w * p.pose.scale * 0.5f / (tanY * aspect)) / PLAY_FILL;
        const V3    eye    = p.pose.at(p.box.size() * 0.5) + p.pose.normal * dist;
        const Quat  rot    = Quat::fromBasis(p.pose.right, -p.pose.down, p.pose.normal);
        const float k      = m_play.framed ? 1.f - std::exp(-dt * 12.f) : 1.f;
        m_play.eye         = lerp(m_play.eye, eye, k);
        m_play.rot         = slerp(m_play.rot, rot, k);
        m_play.framed      = true;
        m_play.framedKey   = p.key;
    }
    if (!m_play.on)
        return;

    // drawn over the world while played
    for (auto& q : m_panels)
        if (playedWith(q, w))
            q.front = true;

    // on resize keep the pointer's offset from the top left (apps lay out from there)
    if (const Vector2D size = drawnSize(p); size != m_play.box) {
        m_play.box = size;
        clampPlayPointer();
    }

    // in 3D only warps move Hyprland's cursor: follow them
    if (const Vector2D g = hypr::pointer()->position(); g != m_play.lastGlobal) {
        m_play.lastGlobal = g;
        const auto hl     = p.hitRoot ? Desktop::View::CWLSurface::fromResource(p.hitRoot) : nullptr;
        if (const auto box = hl ? hl->getSurfaceBoxGlobal() : std::nullopt) {
            m_play.pointer = (g - box->pos() + p.hitOffset) / p.hitScale;
            clampPlayPointer();
        }
    }
}

// Super+wheel while playing here (up: notches < 0); not on the wall or pinned
void CDesktop3D::sizePlayed(int notches) {
    const auto w = m_play.window.lock();
    if (!w || notches == 0)
        return;
    const uintptr_t   key  = reinterpret_cast<uintptr_t>(w.get());
    const std::string name = clipped(w->m_title.empty() ? w->m_class : w->m_title, 40);
    std::string       told;
    if (m_tiling.on && std::ranges::contains(m_tiling.order, key) && !atRing()) {
        if (!std::exchange(m_play.wallTold, true))
            logf("Super+wheel: {} is in tiling mode's row, away from you (the ring stays: Y), as big as the ring has it: nothing to make bigger or smaller", name);
        return;
    }
    if (m_tiling.on && std::ranges::contains(m_tiling.order, key)) {
        // rounded so five notches from 0.5 give exactly 0.25
        const float was = m_play.size;
        m_play.size     = std::clamp(std::round((was - (float)notches * PLAY_SIZE_STEP) * 1e4f) / 1e4f, PLAY_SIZE_MIN, PLAY_SIZE_MAX);
        m_play.centre   = true;
        // less than the size only when the row is out of room
        const float share = playedShare();
        const char* end   = notches < 0 && m_play.size >= PLAY_SIZE_MAX ? " (the most)" : notches > 0 && m_play.size <= PLAY_SIZE_MIN ? " (the least)" : "";
        told              = std::format("{} takes {:.0f}% of the view{}", name, share * 100.f, end);
        if (m_play.size != was)
            logf("Super+wheel: the window played takes {:.0f}% of the view in the ring{}: {}", m_play.size * 100.f,
                 std::abs(share - m_play.size) > 0.005f ? std::format(" ({:.0f}% as it's laid out: the row has no more room)", share * 100.f) : "", name);
    } else if (const auto it = m_placements.find(key); it != m_placements.end() && !it->second.pinned && !it->second.returning && !it->second.follows) {
        SPlacement& pl   = it->second;
        const float wall = m_screen.scale(), was = pl.targetScale / wall;
        pl.targetScale   = wall * std::clamp(was * std::pow(WHEEL_SIZE, (float)notches), std::min(was, 0.2f), std::max(was, 5.f)); // carry wheel's bounds
        told             = std::format("{} made {} where it is: it looks {:.2f} times as big as on your screen", name, notches < 0 ? "bigger" : "smaller",
                                       apparentSize(pl.targetCenter, pl.targetScale));
        logf("Super+wheel: {}", told);
    } else {
        if (!std::exchange(m_play.wallTold, true))
            logf("Super+wheel: {} is {}, as big as it goes there: nothing to make bigger or smaller", name, it != m_placements.end() && it->second.pinned ? "pinned to the view (pin_size)" : "on the desktop wall");
        return;
    }
    m_play.sizeTell = told;
    m_play.sizeAt   = std::chrono::steady_clock::now();
}

// view share of the window played here in the row (0 = none); below m_play.size if out of room
float CDesktop3D::playedShare() const {
    const auto w  = m_play.on ? playedInRow() : nullptr;
    const auto at = std::ranges::find(m_tiling.order, reinterpret_cast<uintptr_t>(w.get()));
    if (!w || at == m_tiling.order.end())
        return 0.f;
    const auto&  r    = m_tiling.ring;
    const auto   in   = tileSizes(m_tiling.order);
    const size_t i    = at - m_tiling.order.begin();
    const auto   laid = layoutRing(r, in);
    return viewShare(r, in[i], laid[i].scale);
}

// pointer kept in the zwp_confined_pointer_v1 region, else on the window and its popups
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
    // within both the drawn buffer and the window box
    const Vector2D drawn = drawnSize(o);
    Vector2D       lo{0, 0}, hi{std::min(o.box.w, drawn.x), std::min(o.box.h, drawn.y)};
    for (const auto& p : m_panels) {
        if (p.kind == PANEL_LAYER || !playedWith(p, w) || &p == &o)
            continue;
        const Vector2D at = p.box.pos() - o.box.pos();
        lo                = {std::min(lo.x, at.x), std::min(lo.y, at.y)};
        hi                = {std::max(hi.x, at.x + p.box.w), std::max(hi.y, at.y + p.box.h)};
    }
    m_play.pointer = {std::clamp(m_play.pointer.x, lo.x, hi.x - 0.01), std::clamp(m_play.pointer.y, lo.y, hi.y - 0.01)};
}

// hit test: topmost popup of the window or its dialogs, else a dialog, else the window
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

// play-mode motion as CInputManager::onMouseMoved: relative motion always, the pointer unless locked
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
        updatePointer(e.timeMs, false, true);
    }
    pointerFrame(true);
}

std::string CDesktop3D::playStatus() const {
    const auto w = m_play.window.lock();
    if (!m_play.on || !w)
        return "null";
    const bool locked = g_pInputManager->isLocked();
    // pointer to 0.01, so one clamped 0.01 inside the edge doesn't read as outside
    return std::format(R"({{"class": "{}", "title": "{}", "pointer": [{:.2f}, {:.2f}], "locked": {}, "confined": {}, "view": {:.2f}, "fill": {}, "size": {:.4f}}})",
                       jsonEscape(w->m_class), jsonEscape(w->m_title), m_play.pointer.x, m_play.pointer.y, locked, !locked && g_pInputManager->isConstrained(), m_play.t,
                       m_play.fill, m_play.size);
}

// the app's cursor on the panel under the pointer
void CDesktop3D::addAppCursor() {
    static auto PINVISIBLE = CConfigValue<Config::INTEGER>("cursor:invisible");
    m_cursorShown          = false;
    if (!m_pointerAt.panel || !m_hookSoftCursor || *PINVISIBLE || m_menu.visible() || (!g_pSeatManager->m_state.pointerFocus && !(PROTO::data && PROTO::data->dndActive())))
        return;
    const auto tex  = hypr::pointer()->getCurrentCursorTexture();
    const auto size = hypr::pointer()->cursorSizeLogical();
    if (!tex || size.x < 1 || size.y < 1)
        return;
    for (auto& p : m_panels) {
        if (p.key != m_pointerAt.panel)
            continue;
        SPanelSurface c;
        c.tex = tex;
        c.box = CBox{m_pointerAt.local - hypr::pointer()->hotspot(), size};
        p.surfaces.emplace_back(std::move(c));
        m_cursorShown = true;
        return;
    }
}

// ------------------------------------------------------- layers over the view

// the 3D monitor's layer surface with keyboard focus (on it, a subsurface or popup)
PHLLS CDesktop3D::keyboardLayer() const {
    const auto mon   = m_monitor.lock();
    const auto focus = g_pSeatManager->m_state.keyboardFocus.lock();
    if (!mon || !focus)
        return nullptr;
    for (const auto& layer : mon->m_layerSurfaceLayers)
        for (const auto& ref : layer) {
            const auto ls = ref.lock();
            if (!ls || !ls->m_mapped || !ls->wlSurface() || !ls->wlSurface()->resource())
                continue;
            bool has = false;
            ls->wlSurface()->resource()->breadthfirst([&](SP<CWLSurfaceResource> s, const Vector2D&, void*) { has = has || s == focus; }, nullptr);
            if (!has && ls->m_popupHead)
                ls->m_popupHead->breadthfirst(
                    [&](SP<Desktop::View::CPopup> p, void*) { has = has || (p && p->wlSurface() && p->wlSurface()->resource() == focus); }, nullptr);
            if (has)
                return ls;
        }
    return nullptr;
}

// a layer surface taking the keyboard goes over the 3D view with its own pointer
void CDesktop3D::updateShell(float dt) {
    const PHLLS wants = m_mode == MODE_ACTIVE && !m_away ? keyboardLayer() : nullptr;
    if (wants) {
        if (!m_shell.input) {
            releaseButtons();
            m_keys.fill(false);
            m_typing = false;
            m_menu.hide();
            playEnded("a layer surface took the keyboard");
            if (m_hold.key)
                place();
            if (const auto mon = m_monitor.lock())
                m_shell.pointer = mon->m_size * 0.5;
            m_aimed = -1;
            m_aimSurface.reset();
            m_pointerAt   = {};
            m_cursorShown = false;
            log(std::format("{} has the keyboard: over the 3D view till it lets go", wants->m_namespace.empty() ? std::string("a layer surface") : wants->m_namespace));
        }
        m_shell.layer   = wants;
        m_shell.input   = true;
        m_shell.closing = 0;
    } else if (m_shell.input) {
        m_shell.input = false;
        releaseButtons();
        g_pSeatManager->setPointerFocus(nullptr, {});
        m_lastSentLocal = {-1, -1};
        m_keys.fill(false);
        log(m_held.on ? "the layer surface let go of the keyboard: a game's keys still held back" : "the layer surface let go of the keyboard: walking again");
    } else if (m_shell.layer) {
        const auto ls = m_shell.layer.lock();
        m_shell.closing += dt;
        if (!ls || !ls->m_mapped || m_shell.closing > 0.5f || m_mode != MODE_ACTIVE || m_away)
            m_shell.layer.reset();
    }

    // not on the desktop wall meanwhile
    const auto ls = m_shell.layer.lock();
    if (!ls)
        return;
    std::unordered_set<uintptr_t> keys{reinterpret_cast<uintptr_t>(ls.get())};
    if (ls->m_popupHead)
        ls->m_popupHead->breadthfirst([&](SP<Desktop::View::CPopup> p, void*) { keys.insert(reinterpret_cast<uintptr_t>(p.get())); }, nullptr);
    std::erase_if(m_panels, [&](const SPanel& p) { return (p.kind == PANEL_LAYER || p.kind == PANEL_POPUP) && keys.contains(p.key); });
}

// pointer focus: the layer's popup or the layer under the pointer, or the held button's surface
void CDesktop3D::shellPointer(uint32_t timeMs) {
    const auto ls  = m_shell.layer.lock();
    const auto mon = m_monitor.lock();
    if (!ls || !mon)
        return;
    const Vector2D         global = mon->m_position + m_shell.pointer;
    SP<CWLSurfaceResource> surf;
    Vector2D               local;
    if (const auto held = m_shell.grab.lock(); held && !m_sentButtons.empty()) {
        surf  = held;
        local = global - m_shell.grabAt;
    } else {
        m_shell.grab.reset();
        if (ls->m_popupHead)
            if (const auto popup = ls->m_popupHead->at(global, true); popup && popup->wlSurface() && popup->wlSurface()->resource()) {
                surf  = popup->wlSurface()->resource();
                local = global - popup->coordsGlobal();
            }
        if (!surf) {
            std::vector<PHLLSREF> just{m_shell.layer};
            PHLLS                 found;
            surf = hypr::layerSurfaceAt(global, &just, &local, &found);
        }
    }
    if (surf != g_pSeatManager->m_state.pointerFocus.lock()) {
        g_pSeatManager->setPointerFocus(surf, local);
        m_lastSentLocal = local;
        if (surf)
            g_pSeatManager->sendPointerFrame();
        return;
    }
    if (surf && local != m_lastSentLocal) {
        g_pSeatManager->sendPointerMotion(timeMs ? timeMs : nowMs(), local);
        g_pSeatManager->sendPointerFrame();
        m_lastSentLocal = local;
    }
}

// moves like Hyprland's cursor, clamped to the monitor
void CDesktop3D::shellMotion(const IPointer::SMotionEvent& e) {
    static auto PNOACCEL = CConfigValue<Config::INTEGER>("input:force_no_accel");
    const auto  mon      = m_monitor.lock();
    if (!mon)
        return;
    PROTO::relativePointer->sendRelativeMotion((uint64_t)e.timeMs * 1000, e.delta, e.unaccel);
    m_shell.pointer += *PNOACCEL == 1 ? e.unaccel : e.delta;
    m_shell.pointer = {std::clamp(m_shell.pointer.x, 0.0, mon->m_size.x - 0.01), std::clamp(m_shell.pointer.y, 0.0, mon->m_size.y - 0.01)};
    shellPointer(e.timeMs);
}

// the layer drawn as Hyprland draws it on the 2D desktop, then its popups
void CDesktop3D::drawShell(const PHLMONITOR& mon) {
    const auto ls = m_shell.layer.lock();
    if (!ls || !ls->m_mapped || !ls->wlSurface() || !ls->wlSurface()->resource())
        return;
    static auto PBLUR      = CConfigValue<Config::INTEGER>("decoration:blur:enabled");
    static auto PDIMAROUND = CConfigValue<Config::FLOAT>("decoration:dim_around");
    auto&       pass       = g_pHyprRenderer->m_renderPass;
    if (*PDIMAROUND && ls->m_ruleApplicator->dimAround().valueOrDefault()) {
        CRectPassElement::SRectData dim;
        dim.box   = {0, 0, mon->m_transformedSize.x, mon->m_transformedSize.y};
        dim.color = CHyprColor(0, 0, 0, *PDIMAROUND * hypr::alpha(ls));
        pass.add(makeUnique<CRectPassElement>(dim));
    }
    const auto                       root = ls->wlSurface()->resource();
    CSurfacePassElement::SRenderData data = {mon, m_frameTime, hypr::realPosition(ls)->value()};
    data.fadeAlpha                        = hypr::alpha(ls);
    data.blur                             = *PBLUR && ls->m_ruleApplicator->blur().valueOrDefault();
    data.surface                          = root;
    data.decorate                         = false;
    data.w                                = hypr::realSize(ls)->value().x;
    data.h                                = hypr::realSize(ls)->value().y;
    data.pLS                              = ls;
    data.blockBlurOptimization            = true;
    data.clipBox                          = CBox{0, 0, mon->m_size.x, mon->m_size.y}.scale(mon->m_scale);
    if (data.blur && ls->m_ruleApplicator->ignoreAlpha().hasValue()) {
        data.discardMode |= DISCARD_ALPHA;
        data.discardOpacity = ls->m_ruleApplicator->ignoreAlpha().valueOrDefault();
    }
    root->breadthfirst(
        [&](SP<CWLSurfaceResource> s, const Vector2D& offset, void*) {
            if (!s->m_current.texture || s->m_current.size.x < 1 || s->m_current.size.y < 1)
                return;
            data.localPos    = offset;
            data.texture     = s->m_current.texture;
            data.surface     = s;
            data.mainSurface = s == root;
            pass.add(makeUnique<CSurfacePassElement>(data));
            data.surfaceCounter++;
        },
        nullptr);
    data.squishOversized = false;
    data.dontRound       = true;
    data.popup           = true;
    data.blur            = *PBLUR && ls->m_ruleApplicator->blurPopups().valueOrDefault();
    data.discardMode &= ~DISCARD_ALPHA;
    data.discardOpacity = 0.F;
    data.surfaceCounter = 0;
    if (ls->m_popupHead)
        ls->m_popupHead->breadthfirst(
            [&](SP<Desktop::View::CPopup> popup, void*) {
                if (!popup || !popup->aliveAndVisible() || !popup->wlSurface())
                    return;
                const auto s = popup->wlSurface()->resource();
                if (!s || !s->m_current.texture || s->m_current.size.x < 1 || s->m_current.size.y < 1)
                    return;
                data.localPos    = popup->coordsRelativeToParent();
                data.texture     = s->m_current.texture;
                data.surface     = s;
                data.mainSurface = false;
                pass.add(makeUnique<CSurfacePassElement>(data));
                data.surfaceCounter++;
            },
            nullptr);
}

// ------------------------------------------------------------------ input

bool CDesktop3D::onRelativeMotion(const IPointer::SMotionEvent& e) {
    if (m_mode == MODE_OFF || m_away)
        return false;
    if (m_mode == MODE_ACTIVE && m_shell.input) {
        shellMotion(e);
        return true;
    }
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
    // a layer over the view: a tablet covers the monitor
    if (m_mode == MODE_ACTIVE && m_shell.input) {
        if (const auto mon = m_monitor.lock()) {
            m_shell.pointer = abs * mon->m_size;
            shellPointer();
        }
        m_lastAbs = abs;
        return true;
    }
    // play mode: a tablet (or a nested session) covers the played window
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
    // absolute devices (tablets, nested sessions) turn the view
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
    // shortcuts act on the focused window: noted for the shortcut hold (onWindowClose)
    const bool shortcut = !modifierKey(k) && (meta || ((mods & HL_MODIFIER_CTRL) && (mods & HL_MODIFIER_ALT)) || ((mods & HL_MODIFIER_ALT) && k == K_F4));
    const auto sentTo   = [this] {
        m_shortcutFor = Desktop::focusState()->window();
        m_shortcutAt  = std::chrono::steady_clock::now();
    };

    // away: keys go to the other monitor's desktop; Super+Esc comes back into 3D
    if (m_away) {
        if (k == K_ESC && meta && m_mode == MODE_ACTIVE) {
            setAway(false);
            info.cancelled = true;
            m_consumed.insert(k);
        }
        return;
    }

    // a layer surface with the keyboard gets every key, as on the 2D desktop
    if (m_shell.input && m_mode == MODE_ACTIVE) {
        if (shortcut)
            sentTo();
        return;
    }

    if (m_typing) {
        if (k == K_ESC && meta) {
            setTyping(false);
            info.cancelled = true;
            m_consumed.insert(k);
        } else if (shortcut)
            sentTo();
        else if (k == K_SPACE)
            m_fpTap = (m_fpTapRight = !m_fpTapRight) ? 1 : 0;
        else
            m_fpTap = keyHand(k);
        return;
    }

    // Super+Esc: release a game's held keys, else go away to another monitor
    if (k == K_ESC && meta && m_mode == MODE_ACTIVE && (m_held.on || desktopSpot())) {
        info.cancelled = true;
        m_consumed.insert(k);
        if (m_held.on)
            setPlay(false);
        else
            setAway(true);
        return;
    }

    // keep compositor shortcuts working, for what the crosshair is on (focusForShortcut)
    if (k == K_LMETA || k == K_RMETA || k == K_LALT || k == K_RALT)
        return;
    if (meta || ((mods & HL_MODIFIER_CTRL) && (mods & HL_MODIFIER_ALT))) {
        if (m_mode == MODE_ACTIVE && k != K_LCTRL && k != K_RCTRL && k != K_LSHIFT && k != K_RSHIFT) {
            focusForShortcut();
            sentTo();
        }
        return;
    }

    info.cancelled = true;
    m_consumed.insert(k);

    if (m_mode != MODE_ACTIVE)
        return;

    // a game's keys held back: only walking and P work
    if (m_held.on) {
        if (walkKey(k)) {
            m_keys[k] = true;
            return;
        }
        m_held.at = std::chrono::steady_clock::now();
        if (k == K_P)
            if (const std::string r = playHeldGame(configuredPlayFill() != bool(mods & HL_MODIFIER_SHIFT)); r.starts_with("error: "))
                notify(r.substr(7), true);
        return;
    }

    // VRChat gestures: F1-F8, shift picks the hand (neither: both)
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
        return;

    switch (k) {
        case K_ESC:
            if (m_hold.key)
                cancelHold();
            else
                exit();
            return;
        case K_G:
        case K_H: // Shift+H pins or unpins
            if (const std::string r = k == K_H && (mods & HL_MODIFIER_SHIFT) ? togglePin() : carry(); r.starts_with("error: "))
                notify(r.substr(7), true);
            return;
        case K_X: // back to the wall; its class opens there again
            if (m_hold.key) {
                const uintptr_t key = m_hold.key;
                if (const auto it = m_placements.find(key); it != m_placements.end())
                    if (const auto w = it->second.window.lock())
                        logf("X: {} back to the wall", classOf(w));
                m_hold = {};
                forgetSpot(key);
                returnToWall(key);
            } else if (m_aimed >= 0 && m_panels[m_aimed].kind != PANEL_LAYER) {
                const auto w = m_panels[m_aimed].window.lock();
                if (w) {
                    logf("X: {} back to the wall", classOf(w));
                    forgetSpot(reinterpret_cast<uintptr_t>(w.get()));
                    returnToWall(reinterpret_cast<uintptr_t>(w.get()));
                }
            }
            return;
        case K_E:
        case K_ENTER:
            if (const std::string r = setTyping(true); r.starts_with("error: "))
                notify(r.substr(7), true);
            return;
        case K_P: // Shift+P: the other view
            if (const std::string r = setPlay(true, configuredPlayFill() != bool(mods & HL_MODIFIER_SHIFT)); r.starts_with("error: "))
                notify(r.substr(7), true);
            return;
        case K_T: // Shift+T: the ring round you here
            if (const std::string r = (mods & HL_MODIFIER_SHIFT) ? tileHere() : setTiling(!m_tiling.on); r.starts_with("error: "))
                notify(r.substr(7), true);
            return;
        case K_Y: // the ring follows or stays
            if (const std::string r = setTileFollow(!m_tiling.follow); r.starts_with("error: "))
                notify(r.substr(7), true);
            return;
        case K_Q: m_menu.show("apps"); return;
        case K_B: m_menu.show("windows"); return;
        case K_R: resetPlayer(); return;
        case K_F: // keeps momentum
            m_fly = !m_fly;
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
        pointerFrame();
        if (m_sentButtons.empty())
            m_drag = {};
        return;
    }

    if (m_mode == MODE_OFF || m_away)
        return;
    if (info)
        info->cancelled = true;

    // clicks go to what's under the layer's pointer; one outside a menu's grab closes it
    if (m_mode == MODE_ACTIVE && m_shell.input) {
        if (!pressed)
            return;
        shellPointer(timeMs);
        const auto surf = g_pSeatManager->m_state.pointerFocus.lock();
        if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(surf)) {
            g_pSeatManager->setGrab(nullptr);
            return;
        }
        const auto mon = m_monitor.lock();
        if (!surf || !mon)
            return;
        g_pSeatManager->sendPointerButton(timeMs, button, WL_POINTER_BUTTON_STATE_PRESSED);
        pointerFrame();
        m_sentButtons.insert(button);
        if (!m_shell.grab) {
            m_shell.grab   = surf;
            m_shell.grabAt = mon->m_position + m_shell.pointer - m_lastSentLocal;
        }
        return;
    }

    // a game's clicks are held back too; clicking its window plays it again
    if (m_mode == MODE_ACTIVE && m_held.on) {
        if (!pressed)
            return;
        if (button == BTN_LEFT_ && m_aimed >= 0 && m_aimed < (int)m_panels.size() && m_panels[m_aimed].kind != PANEL_LAYER) {
            const auto w = topParent(m_panels[m_aimed].window.lock());
            if (w && sameGame(w) && playable(w)) {
                if (const std::string r = setPlay(true, m_play.fill); r.starts_with("error: "))
                    notify(r.substr(7), true);
                return;
            }
        }
        m_held.at = std::chrono::steady_clock::now();
        return;
    }

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

    // crosshair on no window: a left click attacks, or closes an open menu
    if (pressed && m_mode == MODE_ACTIVE && m_aimed < 0 && !m_play.on) {
        if (g_pSeatManager->m_seatGrab)
            g_pSeatManager->setGrab(nullptr);
        else if (button == BTN_LEFT_ && !m_typing && m_avatar)
            m_anim.attack();
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

    if (p.kind != PANEL_LAYER)
        m_shortcutHold.on = false; // shortcuts follow the crosshair again
    if (p.kind == PANEL_WINDOW) {
        const auto w = p.window.lock();
        if (w && w != Desktop::focusState()->window())
            Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK, surf);
    }

    if (!surf)
        return;

    updatePointer(); // focus may have moved: resend the position
    g_pSeatManager->sendPointerButton(timeMs, button, WL_POINTER_BUTTON_STATE_PRESSED);
    pointerFrame();
    m_sentButtons.insert(button);
    m_fpPress = true; // first-person finger poke

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
    const bool shell = m_mode == MODE_ACTIVE && m_shell.input;

    // a game's wheel is held back too
    if (m_mode == MODE_ACTIVE && m_held.on && !shell) {
        m_held.at = std::chrono::steady_clock::now();
        return;
    }

    // Super+wheel resizes the window played here, in whole notches
    if (m_mode == MODE_ACTIVE && m_play.on && !shell && (g_pInputManager->getModsFromAllKBs() & HL_MODIFIER_META)) {
        if (e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL && !m_play.fill) {
            const float notches = e.deltaDiscrete != 0 ? e.deltaDiscrete / 120.f : (float)e.delta / 15.f;
            if (std::signbit(notches) != std::signbit(m_play.wheel))
                m_play.wheel = 0;
            m_play.wheel += notches;
            const int whole = (int)m_play.wheel;
            m_play.wheel -= (float)whole;
            sizePlayed(whole);
        }
        return;
    }

    // the Action Menu: the wheel goes round it, a notch an item
    if (m_mode == MODE_ACTIVE && m_menu.open() && !shell) {
        if (e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            menuWheel(m_menu, m_menuWheel, e.deltaDiscrete != 0 ? e.deltaDiscrete / 120.f : (float)e.delta / 15.f);
        return;
    }

    // carrying: the wheel scales (up: bigger), Ctrl+wheel moves it away (up) or closer
    if (m_mode == MODE_ACTIVE && m_hold.key && !shell && e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        const float    notches = e.deltaDiscrete != 0 ? e.deltaDiscrete / 120.f : (float)e.delta / 15.f;
        const uint32_t mods    = g_pInputManager->getModsFromAllKBs();
        if (mods & HL_MODIFIER_CTRL)
            m_hold.dist = std::clamp(m_hold.dist * std::pow(0.9f, notches), CARRY_NEAREST, 12.f);
        else if (mods & HL_MODIFIER_SHIFT) { // resizes the app itself (text keeps its size)
            if (const auto it = m_placements.find(m_hold.key); it != m_placements.end())
                if (const auto w = it->second.window.lock())
                    resizeReal(w, hypr::realSize(w)->goal() * std::pow((double)WHEEL_SIZE, notches));
        } else
            m_hold.scaleMul = std::clamp(m_hold.scaleMul * std::pow(WHEEL_SIZE, notches), 0.2f, 5.f);
        return;
    }

    // third person, not pointing at anything that scrolls: the wheel zooms
    if (m_mode == MODE_ACTIVE && m_thirdPerson && !m_play.on && !shell && m_aimSurface.expired() && e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        const float notches = e.deltaDiscrete != 0 ? e.deltaDiscrete / 120.f : (float)e.delta / 15.f;
        m_camDist           = std::clamp(m_camDist * std::pow(1.12f, notches), 0.8f, 10.f);
        return;
    }

    if (m_mode != MODE_ACTIVE || (shell ? !g_pSeatManager->m_state.pointerFocus : m_aimSurface.expired()))
        return;

    // as CInputManager::onMouseWheel: rule/device/input scroll factor, input:emulate_discrete_scroll
    static auto PSCROLL   = CConfigValue<Config::FLOAT>("input:scroll_factor");
    static auto PTPSCROLL = CConfigValue<Config::FLOAT>("input:touchpad:scroll_factor");
    static auto PEMULATE  = CConfigValue<Config::INTEGER>("input:emulate_discrete_scroll");
    const bool  touchpad  = *PTPSCROLL <= 0.f || e.source == WL_POINTER_AXIS_SOURCE_FINGER;
    double      factor    = touchpad ? *PTPSCROLL : *PSCROLL;
    if (const auto dev = g_wheelPointer.lock(); dev && dev->m_scrollFactor.has_value())
        factor = *dev->m_scrollFactor;
    if (!shell && m_aimed >= 0 && m_aimed < (int)m_panels.size()) {
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
    // touchpads: wait for the device's frame so both scroll axes share one, as Hyprland does
    m_axisFramePending = (e.source == WL_POINTER_AXIS_SOURCE_FINGER || e.source == WL_POINTER_AXIS_SOURCE_CONTINUOUS) && m_hookFrame;
    if (!m_axisFramePending)
        pointerFrame();
}

void CDesktop3D::onPointerFrame() {
    if (!m_axisFramePending)
        return;
    m_axisFramePending = false;
    pointerFrame();
}

// --------------------------------------------------------- apps and windows

namespace {
    std::string lowered(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    // absolute path without . and .. ("" stays "")
    std::string normalPath(const std::string& p) {
        std::error_code ec;
        return p.empty() ? p : std::filesystem::absolute(p, ec).lexically_normal().string();
    }

    std::string fileSize(uintmax_t bytes) {
        return bytes >= (1ull << 30) ? std::format("{:.1f} GB", bytes / double(1ull << 30))
            : bytes >= (1ull << 20)  ? std::format("{:.0f} MB", bytes / double(1ull << 20))
                                     : std::format("{:.0f} KB", std::ceil(bytes / 1024.0));
    }

    std::string classOf(const PHLWINDOW& w) {
        return w->m_class.empty() ? w->m_initialClass : w->m_class;
    }

    // touch-typing hand for an evdev key on QWERTY: 0 left, 1 right
    int keyHand(uint32_t k) {
        if (k == K_ESC || k == 41 || k == K_TAB || k == 58 || k == K_LSHIFT || k == K_LCTRL || k == K_LALT || k == 86)
            return 0;
        if ((k >= 2 && k <= 7) || (k >= K_Q && k <= K_T) || (k >= K_A && k <= K_G) || (k >= 44 && k <= K_B) || (k >= K_F1 && k <= K_F1 + 4))
            return 0;
        return 1;
    }

    // a dialog's parent; for X11 walks WM_TRANSIENT_FOR itself (Hyprland 0.55.2's x11TransientFor() overshoots)
    PHLWINDOW parentOf(const PHLWINDOW& w) {
        if (!w->m_isX11)
            return w->parent();
        const auto xs = w->m_xwaylandSurface.lock();
        auto       up = xs ? xs->m_parent.lock() : nullptr;
        for (int hops = 0; up && up->m_parent && hops < 16; ++hops)
            up = up->m_parent.lock();
        if (up)
            for (const auto& o : hypr::windows())
                if (o && o != w && o->m_isMapped && o->m_xwaylandSurface.lock() == up)
                    return o;
        return nullptr;
    }

    // a window of its own (not a dialog, a menu or a tooltip)
    bool toplevel(const PHLWINDOW& w) {
        return w && w->m_isMapped && !w->isHidden() && !w->isX11OverrideRedirect() && !parentOf(w);
    }

    // pid and ancestors from /proc/PID/stat (readable where environ isn't, e.g. Chromium)
    std::vector<int64_t> ancestry(int64_t pid) {
        std::vector<int64_t> out;
        while (pid > 1 && out.size() < 64) {
            out.push_back(pid);
            std::ifstream in(std::format("/proc/{}/stat", pid));
            std::string   s;
            const size_t  name = std::getline(in, s) ? s.rfind(')') : std::string::npos; // comm can hold anything
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

// ------------------------------------------------------------- shortcuts, closes, and a game's keys held back

// shortcuts act on the crosshair's or carried window, focused before Hyprland runs the bind; on none when nothing's
// aimed at, during m_shortcutHold, or if focusing would switch workspaces
void CDesktop3D::focusForShortcut() {
    static auto PFALLTHROUGH = CConfigValue<Config::INTEGER>("input:special_fallthrough");
    if (m_mode != MODE_ACTIVE || m_away || m_typing || m_play.on || m_shell.input)
        return;
    PHLWINDOW   w;
    const char* whose = "the crosshair's";
    std::string why   = "the crosshair on no window";
    if (m_shortcutHold.on) {
        why = "a window with the keyboard closed: not turned or moved since";
        // a game window given the keyboard as its popup closed gets it back after the hold (m_giveBack)
        auto f = Desktop::focusState()->window();
        if (f && f->isX11OverrideRedirect())
            f = x11Owner(f);
        if (const auto top = topParent(f); top && ((m_held.on && sameGame(top) && !listed(m_held.others, top)) || fullscreenHere(top)))
            m_giveBack = f;
    } else if (m_menu.open())
        why = "the Action Menu open";
    else if (const auto it = m_hold.key ? m_placements.find(m_hold.key) : m_placements.end(); it != m_placements.end()) {
        w     = it->second.window.lock();
        whose = "the one you carry";
    } else if (m_aimed >= 0 && m_aimed < (int)m_panels.size() && m_panels[m_aimed].kind != PANEL_LAYER)
        w = m_panels[m_aimed].window.lock();
    // an X11 menu or tooltip: its owner, else none (closing a menu can kill its whole app)
    if (w && w->m_isX11 && (w->isX11OverrideRedirect() || w->m_X11ShouldntFocus)) {
        const auto menu = w;
        for (int hops = 0; w && (w->isX11OverrideRedirect() || w->m_X11ShouldntFocus) && hops < 8; ++hops)
            w = x11Owner(w);
        if (w && (w->isX11OverrideRedirect() || w->m_X11ShouldntFocus))
            w = nullptr;
        if (!w)
            why = std::format("{} is an X11 menu of no window's", classOf(menu));
    }
    if (w && (!w->m_workspace || !w->m_workspace->isVisible())) {
        why = std::format("{} is on a workspace not shown", classOf(w));
        w   = nullptr;
    }
    // hidden under an open special workspace though "visible" (unless pinned or with input:special_fallthrough)
    if (const auto mon = w ? w->m_monitor.lock() : nullptr;
        w && !w->m_pinned && !*PFALLTHROUGH && mon && mon->m_activeSpecialWorkspace && mon->m_activeSpecialWorkspace != w->m_workspace) {
        why = std::format("{} is under a special workspace", classOf(w));
        w   = nullptr;
    }
    const auto focus = Desktop::focusState()->window();
    if (w == focus)
        return;
    if (w) {
        // under a fullscreen window: raw focus, as a full focus would end its fullscreen
        if (const auto fs = hypr::fullscreenWindow(w->m_workspace); fs && fs != w)
            Desktop::focusState()->rawWindowFocus(w, Desktop::FOCUS_REASON_CLICK);
        else
            Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK);
        if (Desktop::focusState()->window() != w)
            why = std::format("{} won't take it", classOf(w));
    }
    if (!w || Desktop::focusState()->window() != w)
        Desktop::focusState()->rawWindowFocus(nullptr, Desktop::FOCUS_REASON_OTHER);
    const auto        now = Desktop::focusState()->window();
    const std::string was = focus ? classOf(focus) : "none";
    if (w && now == w)
        logf("a shortcut: the keyboard to {} ({}), was {}", classOf(w), whose, was);
    else if (!now)
        logf("a shortcut: the keyboard to none ({}), was {}", why, was);
    else // a layer surface on another monitor holds it
        logf("a shortcut: the keyboard stays on {} ({}; a layer surface keeps it there)", classOf(now), why);
}

// a focused window closed by a shortcut starts the shortcut hold; the played window closing ends play mode
void CDesktop3D::onWindowClose(const PHLWINDOW& w) {
    if (m_mode == MODE_OFF || !w || w->isX11OverrideRedirect())
        return;
    const auto  pl     = m_placements.find(reinterpret_cast<uintptr_t>(w.get()));
    const bool  placed = pl != m_placements.end() && !pl->second.returning;
    const char* where  = placed ? (pl->second.tiled ? "in the ring" : pl->second.pinned ? "pinned to the view" : "in the world") :
        windowPanel(w) >= 0                         ? "on the wall" :
                                                      "not in the 3D view";
    const bool  had    = w == Desktop::focusState()->window();
    const bool  aimed  = m_aimed >= 0 && m_aimed < (int)m_panels.size() && m_panels[m_aimed].window.lock() == w;
    const bool  played = m_play.on && m_play.window.lock() == w;
    const bool  under  = hypr::blockedBelowFullscreen(w);
    logf("closed: {} ({}), {}{}{}{}{}", classOf(w), clipped(w->m_title, 40), where, under ? ", under a fullscreen window" : "", had ? ", it had the keyboard" : "",
         aimed ? ", the crosshair on it" : "", played ? ", played" : "");
    if (had && m_mode == MODE_ACTIVE && !m_away) {
        if (const auto now = std::chrono::steady_clock::now(); w == m_shortcutFor.lock() && now - m_shortcutAt < std::chrono::duration<float>(SHORTCUT_CLOSE_TIME))
            m_shortcutHold = {.on = true, .yaw = m_yaw, .pitch = m_pitch, .feet = m_body.feet, .at = now};
        // under a fullscreen window: Hyprland would focus the next one on return, ending its fullscreen
        if (under)
            Desktop::focusState()->rawWindowFocus(nullptr, Desktop::FOCUS_REASON_OTHER);
        // Hyprland focuses another window on return: note the game's others
        if ((played || m_held.on) && sameGame(w)) {
            m_held.others.clear();
            for (const auto& o : hypr::windows())
                if (o && o != w && o->m_isMapped && sameGame(o))
                    m_held.others.emplace_back(o);
        }
    }
    if (played)
        playEnded("its window closed");
}

// a window of the game played last: its process's, else one of its class
bool CDesktop3D::sameGame(const PHLWINDOW& w) const {
    if (!w)
        return false;
    if (m_held.pid > 0 && w->getPID() == m_held.pid)
        return true;
    return !m_held.cls.empty() && classOf(w) == m_held.cls;
}

// a toplevel in the 3D view, not tiny nor under a fullscreen window
bool CDesktop3D::playable(const PHLWINDOW& w) const {
    if (!toplevel(w) || windowPanel(w) < 0 || hypr::blockedBelowFullscreen(w))
        return false;
    const Vector2D size = hypr::realSize(w)->goal();
    return size.x >= TILE_MIN_PX && size.y >= TILE_MIN_PX;
}

std::string CDesktop3D::playAgain(const PHLWINDOW& w, bool fill) {
    const int i = windowPanel(w);
    if (i < 0)
        return "error: it isn't in the 3D view";
    m_aimed         = i;
    m_aimPanelLocal = m_panels[i].box.size() * 0.5;
    return setPlay(true, fill);
}

// P with a game's keys held back: its aimed window, else the last played, else another
std::string CDesktop3D::playHeldGame(bool fill) {
    PHLWINDOW aimed;
    if (m_aimed >= 0 && m_aimed < (int)m_panels.size() && m_panels[m_aimed].kind != PANEL_LAYER)
        aimed = topParent(m_panels[m_aimed].window.lock());
    if (!(aimed && sameGame(aimed))) {
        PHLWINDOW game = m_held.window.lock();
        if (!playable(game)) {
            game = nullptr;
            for (const auto& p : m_panels)
                if (const auto o = p.kind == PANEL_WINDOW ? p.window.lock() : nullptr; o && sameGame(o) && playable(o)) {
                    game = o;
                    break;
                }
        }
        if (game)
            return playAgain(game, fill);
    }
    return setPlay(true, fill);
}

// ends the shortcut hold once you turn or move; held game keys return to walking after PLAY_PAUSE_IDLE, or the game
// plays again once its window has the keyboard
void CDesktop3D::updateHolds() {
    const auto now = std::chrono::steady_clock::now();
    if (auto& h = m_shortcutHold; h.on &&
        (m_mode != MODE_ACTIVE || m_away || std::abs(wrapAngle(m_yaw - h.yaw)) > SHORTCUT_HOLD_TURN || std::abs(m_pitch - h.pitch) > SHORTCUT_HOLD_TURN ||
         length(m_body.feet - h.feet) > SHORTCUT_HOLD_MOVE || now - h.at > std::chrono::duration<float>(SHORTCUT_HOLD_TIME)))
        h.on = false;
    // return the keyboard to the game window a shortcut took it from, once the hold ends
    if (const auto g = m_giveBack.lock(); !g || m_mode != MODE_ACTIVE || m_away || Desktop::focusState()->window() || !g->m_isMapped ||
        !((m_held.on && sameGame(topParent(g))) || fullscreenHere(topParent(g))))
        m_giveBack.reset();
    else if (!m_shortcutHold.on && !m_shell.input && g_pInputManager->m_exclusiveLSes.empty() && g_pSeatManager->m_state.keyboardFocus.expired()) {
        m_giveBack.reset();
        if (windowPanel(g) >= 0 && focusable(g)) {
            Desktop::focusState()->fullWindowFocus(g, Desktop::FOCUS_REASON_CLICK);
            if (keyboardWith(topParent(g)))
                logf("the keyboard back to {}, shortcuts held no more", classOf(g));
        }
    }

    for (auto it = m_playHints.begin(); it != m_playHints.end();) {
        if (now - it->at < std::chrono::milliseconds(1500)) {
            ++it;
            continue;
        }
        const SPlayHint h = *it;
        it                = m_playHints.erase(it);
        const auto told   = m_playHinted.find(h.cls);
        const auto played = m_play.window.lock();
        if (m_mode != MODE_ACTIVE || m_away || now - h.at > std::chrono::seconds(5) || (played && classOf(played) == h.cls) || (m_held.on && m_held.cls == h.cls) ||
            (told != m_playHinted.end() && now - told->second < std::chrono::seconds(60)))
            continue; // opened before leaving 3D
        // the first window opened, else a drawn one of its class
        PHLWINDOW w = h.window.lock();
        if (!w || !w->m_isMapped || windowPanel(w) < 0) {
            w = nullptr;
            for (const auto& p : m_panels)
                if (const auto o = p.kind == PANEL_WINDOW ? p.window.lock() : nullptr; o && toplevel(o) && classOf(o) == h.cls && (!w || w->m_title.empty()))
                    w = o;
        }
        if (!w)
            continue;
        m_playHinted[h.cls] = now;
        notify(std::format("{}: P plays it (Super+Esc gives the keys back)", clipped(w->m_title.empty() ? h.cls : w->m_title, 40)));
    }

    if (!m_held.on)
        return;
    if (m_mode != MODE_ACTIVE || m_away || m_play.on) {
        m_held.on = false;
        return;
    }
    // not under a layer surface (it would end play again); idle time starts once it lets go
    if (m_shell.input || !g_pInputManager->m_exclusiveLSes.empty()) {
        m_held.at = now;
        return;
    }
    auto f = Desktop::focusState()->window();
    if (f && f->isX11OverrideRedirect())
        f = x11Owner(f);
    f = topParent(f);
    // a window focused as another game window closed counts only once focus was elsewhere
    std::erase_if(m_held.others, [&](const PHLWINDOWREF& o) { return o.expired() || o.lock() != f; });
    if (f && m_held.others.empty() && sameGame(f) && playable(f)) {
        // not during the shortcut hold: a second Super+Q would close the game
        if (m_shortcutHold.on) {
            m_held.at = now;
            return;
        }
        logf("play mode: {} has the keyboard again", classOf(f));
        if (const std::string r = playAgain(f, m_play.fill); r.starts_with("error: ")) { // in its previous view
            m_held.on = false; // don't retry every frame
            log("play mode: " + r.substr(7));
        }
        return;
    }
    // held until m_giveBack returns the keyboard
    if (!m_giveBack.expired()) {
        m_held.at = now;
        return;
    }
    if (now - m_held.at > std::chrono::duration<float>(PLAY_PAUSE_IDLE)) {
        m_held.on = false;
        notify("walking again: the keys are hyprwalk's");
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

// files with `exts` in $XDG_DATA_HOME/hyprwalk/FOLDER and its subfolders, then `also`
void CDesktop3D::listModelFiles(std::vector<SModelFile>& out, const char* folder, std::initializer_list<std::string_view> exts, std::initializer_list<std::string> also) {
    namespace fs = std::filesystem;
    out.clear();
    const auto isModel = [&](const fs::path& p) {
        std::error_code   ec;
        const std::string ext = lowered(p.extension().string());
        return std::ranges::find(exts, std::string_view(ext)) != exts.end() && fs::is_regular_file(p, ec);
    };
    const auto add = [&](const fs::path& p, std::string label) {
        const std::string abs = normalPath(p.string());
        if (std::ranges::any_of(out, [&](const SModelFile& f) { return f.path == abs; }))
            return;
        std::error_code ec;
        const uintmax_t bytes = fs::file_size(p, ec);
        out.push_back({abs, std::move(label), ec ? 0 : bytes});
    };
    const char*    xdg  = getenv("XDG_DATA_HOME");
    const char*    home = getenv("HOME");
    const fs::path dir  = (xdg && *xdg ? fs::path(xdg) : fs::path(home ? home : "/tmp") / ".local/share") / "hyprwalk" / folder;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (isModel(it->path())) {
            add(it->path(), it->path().stem().string());
            continue;
        }
        std::error_code   inEc;
        std::vector<fs::path> in;
        if (it->is_directory(inEc))
            for (fs::directory_iterator f(it->path(), inEc), fend; !inEc && f != fend; f.increment(inEc))
                if (isModel(f->path()))
                    in.push_back(f->path());
        const std::string sub = it->path().filename().string();
        for (const auto& f : in)
            add(f, in.size() == 1 ? sub : sub + "/" + f.stem().string());
    }
    std::ranges::sort(out, [](const SModelFile& a, const SModelFile& b) { return lowered(a.label) < lowered(b.label); });
    for (const std::string& p : also)
        if (!p.empty() && isModel(p))
            add(p, fs::path(p).stem().string());
}

const std::vector<CDesktop3D::SModelFile>& CDesktop3D::mapFiles() {
    const auto now = std::chrono::steady_clock::now();
    if (m_mapFilesRead != std::chrono::steady_clock::time_point{} && now - m_mapFilesRead < std::chrono::seconds(2))
        return m_mapFiles;
    m_mapFilesRead = now;
    listModelFiles(m_mapFiles, "maps", {".glb", ".gltf"}, {configuredMap(), m_mapPath, m_world.model ? m_world.model->path : std::string()});
    return m_mapFiles;
}

// the Maps page's pick ("" = courtyard); picking the shown map cancels a pending load
std::string CDesktop3D::pickMap(const std::string& path) {
    const std::string shown = m_world.model ? m_world.model->path : "", want = normalPath(path);
    if (want == normalPath(shown) && normalPath(m_mapPath) != want) {
        m_mapLoader.cancel();
        m_mapPath = shown;
        notify(shown.empty() ? "staying in the courtyard" : "staying on " + m_world.name);
        return "ok";
    }
    if (want == normalPath(m_mapPath)) // here already, or on its way
        return m_mapLoader.busy() ? "loading" : "ok";
    const bool isDefault = !want.empty() && want == normalPath(configuredMap());
    return requestMap(want, isDefault && g_cfgMapScale ? g_cfgMapScale->value() : 0.f);
}

// the Avatars page's list (not .vrma: those are emotes)
const std::vector<CDesktop3D::SModelFile>& CDesktop3D::avatarFiles() {
    const auto now = std::chrono::steady_clock::now();
    if (m_avatarFilesRead != std::chrono::steady_clock::time_point{} && now - m_avatarFilesRead < std::chrono::seconds(2))
        return m_avatarFiles;
    m_avatarFilesRead = now;
    listModelFiles(m_avatarFiles, "avatars", {".glb", ".gltf", ".vrm"}, {configuredPath(g_cfgAvatar), m_avatarPath, m_avatar ? m_avatar->path : std::string()});
    return m_avatarFiles;
}

// the Avatars page's pick, at avatar_height; picking the shown one cancels a pending load
std::string CDesktop3D::pickAvatar(const std::string& path) {
    const std::string shown = m_avatar ? normalPath(m_avatar->path) : "", want = normalPath(path);
    if (want.empty())
        return "error: no avatar file";
    if (want == shown && normalPath(m_avatarPath) != want) {
        m_avatarLoader.cancel();
        m_avatarPath = m_avatar->path;
        notify("keeping avatar " + m_avatar->name);
        return "ok";
    }
    if (want == normalPath(m_avatarPath)) // shown already, or on its way
        return m_avatarLoader.busy() ? "loading" : "ok";
    return requestAvatar(want, g_cfgAvatarHeight ? g_cfgAvatarHeight->value() : 0.f);
}

// starts an app like exec, with HYPRWALK_LAUNCH so its window opens in front of you
std::string CDesktop3D::launch(const std::string& what) {
    std::string cmd = what, cls, name = what;
    if (const SAppEntry* e = findApp(apps(), what)) {
        cmd  = e->terminal ? inTerminal(e->exec) : e->exec;
        cls  = e->wmClass.empty() ? e->id : e->wmClass;
        name = e->name;
    } else {
        // a command: class from a desktop entry running the same program, else its name
        std::string        prog;
        std::istringstream in(what);
        for (std::string word; in >> word;) {
            if (word.find('=') != std::string::npos && word.find('/') == std::string::npos)
                continue; // VAR=value
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
    // Steam starts games itself: match SteamAppId=ID in the window's environment, else class steam_app_ID (Proton)
    SLaunch l;
    if (const size_t at = cmd.find("steam://rungameid/"); at != std::string::npos) {
        l.steam = cmd.substr(at + 18, cmd.find_first_not_of("0123456789", at + 18) - (at + 18));
        if (!l.steam.empty())
            cls = "steam_app_" + l.steam;
    }
    l.token = std::format("{}-{}", getpid(), ++m_launchCount);
    l.what  = name;
    l.cls   = cls;
    l.at    = std::chrono::steady_clock::now();
    const auto pid = Config::Supplementary::executor()->spawnRawProc(std::format("export HYPRWALK_LAUNCH={}; {}", l.token, cmd));
    if (!pid || !*pid)
        return "error: couldn't start " + name;
    l.pid = (int64_t)*pid;
    std::erase_if(m_launches, [&](const SLaunch& o) { return l.at - o.at > std::chrono::seconds(60); });
    m_launches.push_back(l);
    notify(std::format("starting {}{}", name, m_mode == MODE_ACTIVE ? ": it opens in front of you" : ""));
    return std::format("launched {} (pid {})", name, *pid);
}

// places a new window at its class's spot if in sight, else in front of you (launched from 3D, or on the 3D monitor)
void CDesktop3D::onWindowOpen(const PHLWINDOW& w) {
    if (!w || w->isX11OverrideRedirect())
        return;
    // a window closed outside 3D can leave state for a new one at its address
    std::erase_if(m_placements, [](const auto& kv) { return kv.second.window.expired(); });
    if (const uintptr_t key = reinterpret_cast<uintptr_t>(w.get()); !m_placements.contains(key)) {
        std::erase(m_tiling.order, key);
        m_tiling.before.erase(key);
        m_tiling.stay.erase(key);
        m_tiling.walled.erase(key);
        m_tiling.kept.erase(key);
        m_tiling.laid.erase(key);
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_launches, [&](const SLaunch& l) { return now - l.at > std::chrono::seconds(60); });
    const std::string cls = classOf(w);
    bool              launched = false;
    const auto        env      = w->getEnv();
    const auto        token    = env.find("HYPRWALK_LAUNCH");
    const auto        steamId  = env.contains("SteamAppId") ? env.at("SteamAppId") : env.contains("SteamGameId") ? env.at("SteamGameId") : "";
    const auto        parents  = m_launches.empty() ? std::vector<int64_t>{} : ancestry(w->getPID());
    // a Steam game's first window: hint how to play it (updateHolds)
    if ((!steamId.empty() || lowered(cls).starts_with("steam_app_")) && m_mode == MODE_ACTIVE && toplevel(w) &&
        std::ranges::none_of(m_playHints, [&](const SPlayHint& h) { return h.cls == cls; }))
        m_playHints.push_back({.cls = cls, .window = w, .at = now});
    for (auto it = m_launches.begin(); it != m_launches.end(); ++it) {
        bool ours = false, surely = false; // surely: by process, all its windows
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
        return;
    // a dialog of a window in the world: just in front of its parent
    if (const auto parent = parentOf(w); parent && w->m_isMapped) {
        const auto pp = m_placements.find(reinterpret_cast<uintptr_t>(parent.get()));
        if (pp == m_placements.end() || pp->second.returning)
            return;
        if (pp->second.pinned) { // parent pinned to the view
            placeInFront(w, {});
            return;
        }
        const SPlacement& o  = pp->second;
        const Vector2D    at = (hypr::realPosition(w)->goal() + hypr::realSize(w)->goal() * 0.5) - (hypr::realPosition(parent)->goal() + hypr::realSize(parent)->goal() * 0.5);
        SPlacement        pl;
        pl.window  = w;
        pl.follows = pp->first; // moves with its parent
        pl.center = pl.targetCenter = o.targetCenter + o.targetRot.rotate({(float)at.x * o.targetScale, -(float)at.y * o.targetScale, 0.06f});
        pl.rot = pl.targetRot = o.targetRot;
        pl.scale = pl.targetScale = o.targetScale;
        m_placements[reinterpret_cast<uintptr_t>(w.get())] = pl;
        return;
    }
    if (!toplevel(w))
        return;
    // tiling mode: windows that would open in front of you join the row where you look
    const bool intoRow = m_tiling.on && m_mode == MODE_ACTIVE && tileable(w) &&
        (launched || (w->m_monitor.lock() == m_monitor.lock() && w->m_workspace && w->m_workspace->isVisible()));
    if (intoRow && atRing()) {
        tileWindow(w, lookSlot());
        m_tiling.stay.insert(reinterpret_cast<uintptr_t>(w.get()));
        logf("{}: into the row", cls);
        return;
    }
    if (intoRow) {
        // stays put when tiling ends, even if carried into the row
        m_tiling.stay.insert(reinterpret_cast<uintptr_t>(w.get()));
        logf("{}: away from the row, not into it", cls);
    }
    loadSpots();
    const auto spot = m_spots.find(cls);
    const bool alone = std::ranges::count_if(hypr::windows(), [&](const PHLWINDOW& o) { return toplevel(o) && classOf(o) == cls; }) == 1;
    // its spot only if you'd see it open there (else it looks like it never opened)
    const bool there = spot != m_spots.end() && (launched || alone);
    if (there && (m_mode != MODE_ACTIVE || inSight(spot->second))) {
        placeAt(w, spot->second);
        logf("{}: opened in its place", cls);
        return;
    }
    if (there)
        logf("{}: not to its place (out of sight)", cls);
    if (launched)
        placeInFront(w, appRule(m_rules, cls));
    else if (m_mode == MODE_ACTIVE && w->m_monitor.lock() == m_monitor.lock() && w->m_workspace && w->m_workspace->isVisible() && hypr::realSize(w)->goal().y >= 1) {
        // other windows on the 3D monitor: in front of you; floating ones nearer at screen size
        placeInFront(w, w->m_isFloating ? SAppRule{.distance = 1.3f} : appRule(m_rules, cls));
    }
    // away from a staying ring, a window left on the wall stays rather than flying off to the row
    if (intoRow && !m_placements.contains(reinterpret_cast<uintptr_t>(w.get())))
        m_tiling.walled.insert(reinterpret_cast<uintptr_t>(w.get()));
}

// fullscreen in 3D means auto-play (autoPlay); leaving fullscreen clears a decline but doesn't end play
void CDesktop3D::onFullscreen(const PHLWINDOW& w) {
    if (!w)
        return;
    if (hypr::fullscreen(w)) {
        if (m_mode != MODE_OFF && !listed(m_fullscreen.fresh, w))
            m_fullscreen.fresh.emplace_back(w);
        return;
    }
    std::erase_if(m_fullscreen.declined, [&](const PHLWINDOWREF& r) { return r.lock() == w; });
    std::erase_if(m_fullscreen.fresh, [&](const PHLWINDOWREF& r) { return r.lock() == w; });
}

// the 3D monitor's fullscreen window, unless declined (autoPlay plays it)
bool CDesktop3D::fullscreenHere(const PHLWINDOW& w) const {
    const auto mon = m_monitor.lock();
    const auto ws  = mon ? mon->m_activeWorkspace : nullptr;
    return w && ws && hypr::fullscreenWindow(ws) == w && hypr::fullscreen(w) && !listed(m_fullscreen.declined, w);
}

// focusing w switches nothing: workspace shown and no special workspace over it
bool CDesktop3D::focusable(const PHLWINDOW& w) const {
    static auto PFALLTHROUGH = CConfigValue<Config::INTEGER>("input:special_fallthrough");
    if (!w || !w->m_isMapped || !w->m_workspace || !w->m_workspace->isVisible())
        return false;
    const auto mon = w->m_monitor.lock();
    return w->m_pinned || *PFALLTHROUGH || !mon || !mon->m_activeSpecialWorkspace || mon->m_activeSpecialWorkspace == w->m_workspace;
}

// auto-plays the 3D monitor's fullscreen window once it (or its dialog) has the keyboard while you just walk; a fresh
// one with nothing focused gets focused
void CDesktop3D::autoPlay() {
    const auto stale = [](const PHLWINDOWREF& r) {
        const auto w = r.lock();
        return !w || !w->m_isMapped || !hypr::fullscreen(w);
    };
    std::erase_if(m_fullscreen.declined, stale);
    std::erase_if(m_fullscreen.fresh, stale);
    const auto mon = m_monitor.lock();
    const auto ws  = mon ? mon->m_activeWorkspace : nullptr;
    const auto w   = ws ? hypr::fullscreenWindow(ws) : nullptr;
    if (m_mode != MODE_ACTIVE || !w || !hypr::fullscreen(w) || m_play.on || m_away || m_shell.input || m_menu.open() || m_hold.key ||
        (m_typing && m_typingInto.lock() != w) || !g_pInputManager->m_exclusiveLSes.empty() || windowPanel(w) < 0 || listed(m_fullscreen.declined, w))
        return; // an exclusive layer surface would end it at once
    if (m_shortcutHold.on || (m_held.on && listed(m_held.others, w)))
        return;
    if (hypr::realSize(w)->isBeingAnimated() || hypr::realPosition(w)->isBeingAnimated())
        return; // turn to its middle once at full size
    const bool  fresh = std::erase_if(m_fullscreen.fresh, [&](const PHLWINDOWREF& r) { return r.lock() == w; }) > 0;
    const char* how   = nullptr;
    if (keyboardWith(w))
        how = "fullscreen, played";
    else if (fresh && !Desktop::focusState()->window() && !mon->m_activeSpecialWorkspace) {
        Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK);
        if (keyboardWith(w))
            how = "fullscreen with no window focused: focused and played";
    }
    if (!how)
        return; // another window has the keyboard
    m_aimed         = windowPanel(w);
    m_aimPanelLocal = m_panels[m_aimed].box.size() * 0.5;
    // a held-back game regaining the keyboard: in its previous view
    if (setPlay(true, m_held.on && sameGame(w) ? std::optional<bool>(m_play.fill) : std::nullopt) == "playing")
        logf("{}: {}", classOf(w), how);
}

// places w in front of you per the rule (no height: as on your screen), pulled nearer and smaller past obstacles
bool CDesktop3D::placeInFront(const PHLWINDOW& w, const SAppRule& rule) {
    const Vector2D size = hypr::realSize(w)->goal();
    if (size.x < 1 || size.y < 1)
        return false;
    const V3 fwd{std::sin(m_yaw), 0, -std::cos(m_yaw)}, right{std::cos(m_yaw), 0, std::sin(m_yaw)};
    V3       eye = playerCamera().eye, from = eye;
    float    back = 0; // how far behind `from` the view is seen from
    if (m_thirdPerson && m_avatar) {
        from = camPivot();
        eye  = from - forwardFrom(m_yaw, m_pitch) * m_camBoom;
        back = std::max(0.f, dot(from - eye, fwd));
    }
    const float ahead = back + rule.distance; // distance from the view
    const float fit   = back > 0.f ? THIRD_FIT : FRONT_FIT;
    float       scale = frontScale(size, ahead, rule.height, fit);
    V3          c     = from + fwd * rule.distance;
    if (back > 0.f) {
        const float ground = m_body.feet.y + 0.05f, reach = fit * ahead * std::tan(FOV_Y * 0.5f);
        if (rule.height <= 0)
            scale = std::min(scale, (eye.y + reach - std::max(eye.y - reach, ground)) / (float)size.y);
        const float half = (float)size.y * scale * 0.5f;
        c.y              = std::max(std::min(c.y, eye.y + reach - half), ground + half);
    }
    float side = rule.side * (ahead / rule.distance); // same angle as from the eye
    if (rule.height <= 0) {
        const float halfView = fit * ahead * std::tan(FOV_Y * 0.5f) * (float)(m_screen.logicalSize.x / m_screen.logicalSize.y);
        side                 = std::copysign(std::min(std::abs(side), std::max(0.f, halfView - (float)size.x * scale * 0.5f)), side);
    }
    c          = c + right * side;
    const V3 n = normalize(V3{eye.x - c.x, 0, eye.z - c.z});
    const V3 r = normalize(cross(V3{0, 1, 0}, n));
    const V3 u = cross(n, r);
    // at least 0.4 m past the eye or the avatar
    const float     nearest = back + 0.4f;
    float           pull    = std::max(clearance(eye, c, r, u, n, (float)size.x * 0.5f * scale, (float)size.y * 0.5f * scale), std::min(1.f, nearest / length(c - eye)));
    const uintptr_t key     = reinterpret_cast<uintptr_t>(w.get());
    // two opened in a row: 10 cm in front of the other
    for (int i = 0; i < 8; ++i) {
        const V3    at    = eye + (c - eye) * pull;
        const float depth = length(at - eye);
        float       front = depth; // how far from the eye it can be
        for (const auto& [k, o] : m_placements)
            if (k != key && !o.returning && !o.pinned && length(o.targetCenter - at) < 0.25f && length(o.targetCenter - eye) - depth < 0.099f)
                front = std::min(front, length(o.targetCenter - eye) - 0.1f);
        if (front >= depth || front < nearest)
            break;
        pull *= front / depth;
    }
    SPlacement pl;
    pl.window = w;
    pl.center = pl.targetCenter = eye + (c - eye) * pull;
    pl.rot = pl.targetRot = Quat::fromBasis(r, u, n);
    pl.scale = pl.targetScale = scale * pull;
    m_placements[key] = pl;
    return true;
}

float CDesktop3D::frontScale(const Vector2D& size, float dist, float height, float fit) const {
    if (height > 0)
        return height / (float)std::max(size.y, 1.0);
    // the view there is 2 tan(FOV_Y / 2) dist metres tall, the monitor's logical height on screen
    const float viewH = 2.f * dist * std::tan(FOV_Y * 0.5f), viewW = viewH * (float)(m_screen.logicalSize.x / m_screen.logicalSize.y);
    return std::min({viewH / (float)m_screen.logicalSize.y, fit * viewH / (float)std::max(size.y, 1.0), fit * viewW / (float)std::max(size.x, 1.0)});
}

float CDesktop3D::apparentSize(const V3& center, float scale) const {
    const float depth = dot(center - m_camera.eye, m_camFwd); // a flat picture shrinks with depth
    return depth > 0.05f ? scale / (2.f * depth * std::tan(FOV_Y * 0.5f) / (float)m_screen.logicalSize.y) : 0.f;
}

float CDesktop3D::clearance(const V3& eye, const V3& c, const V3& r, const V3& u, const V3& n, float hw, float hh) const {
    // anything between you and some point of the window is in the way
    float         pull = 1.f;
    constexpr int NX = 17, NY = 9;
    for (int iy = 0; iy < NY; ++iy) {
        for (int ix = 0; ix < NX; ++ix) {
            const V3    point = c + r * (hw * (2.f * ix / (NX - 1) - 1.f)) + u * (hh * (2.f * iy / (NY - 1) - 1.f)) + n * 0.01f;
            const V3    d     = point - eye;
            const float len   = length(d);
            if (SRayHit hit; len > 1e-3f && m_world.collision.raycast(eye, d / len, len, hit))
                pull = std::min(pull, (hit.t - 0.04f) / len);
        }
    }
    return std::max(pull, 0.1f);
}

// third-person camera boom: the crosshair's ray passes the avatar's head that far ahead
float CDesktop3D::carryFrom() const {
    return m_thirdPerson && m_avatar ? m_camBoom : 0.f;
}

void CDesktop3D::placeAt(const PHLWINDOW& w, const SWindowSpot& spot) {
    SPlacement pl;
    pl.window = w;
    pl.center = pl.targetCenter = spot.center;
    pl.rot = pl.targetRot = spot.rot;
    pl.scale = pl.targetScale = spot.scale;
    m_placements[reinterpret_cast<uintptr_t>(w.get())] = pl;
}

// would you see a window open there: in view, facing you, big enough, unobstructed
bool CDesktop3D::inSight(const SWindowSpot& spot) const {
    const V3    eye = m_camera.eye, d = spot.center - eye;
    const float depth = dot(d, m_camFwd), len = length(d);
    if (depth < 0.3f || dot(spot.rot.rotate({0, 0, 1}), eye - spot.center) <= 0.f || apparentSize(spot.center, spot.scale) < 0.25f)
        return false;
    const float tanY = std::tan(FOV_Y * 0.5f), aspect = (float)(m_screen.logicalSize.x / std::max(m_screen.logicalSize.y, 1.0));
    const V3    right = normalize(cross(m_camFwd, m_camUp));
    if (std::abs(dot(d, m_camUp)) > depth * tanY || std::abs(dot(d, right)) > depth * tanY * aspect)
        return false;
    SRayHit hit;
    return !m_world.collision.raycast(eye, d / len, len - 0.05f, hit);
}

void CDesktop3D::loadSpots() {
    if (m_spotsFor == m_mapPath)
        return;
    m_spots    = readWindowSpots(m_mapPath);
    m_spotsFor = m_mapPath;
}

// remembers where a window was put, per class and map
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

// entering 3D: windows go to their class's spot if alone in their class
void CDesktop3D::restoreSpots() {
    std::erase_if(m_placements, [](const auto& kv) { return kv.second.window.expired(); });
    loadSpots();
    if (m_spots.empty())
        return;
    std::unordered_map<std::string, int> count;
    for (const auto& w : hypr::windows())
        if (toplevel(w))
            ++count[classOf(w)];
    for (const auto& w : hypr::windows()) {
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

// ------------------------------------------------------------ tiling mode

// T: the windows side by side in a ring round you (tiling.cpp); off, each goes back where it was
std::string CDesktop3D::setTiling(bool on) {
    if (on == m_tiling.on)
        return on ? "tiling" : "not tiling";
    if (on) {
        m_tiling.on = true;
        forgetTiles(); // ring round you once in 3D
        if (m_mode == MODE_ACTIVE) {
            anchorRing();
            gatherTiles();
            const size_t n = m_tiling.order.size();
            notify(std::format("tiling: {} window{} round you, {}", n, n == 1 ? "" : "s",
                               m_tiling.follow ? "going with you. T again puts them back, Shift+T turns the row to where you look, Y leaves it here"
                                               : "staying here. T again puts them back, Shift+T brings the row round you, Y takes it with you"));
        }
        log("tiling on");
        return "tiling";
    }
    for (const uintptr_t key : m_tiling.order) {
        const auto it = m_placements.find(key);
        if (it == m_placements.end())
            continue;
        SPlacement& pl = it->second;
        pl.tiled       = false;
        if (const auto b = m_tiling.before.find(key); b != m_tiling.before.end()) {
            pl.targetCenter = b->second.targetCenter;
            pl.targetRot    = b->second.targetRot;
            pl.targetScale  = b->second.targetScale;
        } else if (!m_tiling.stay.contains(key))
            pl.returning = true; // was on the wall
    }
    // the one carried out of the row: escape puts it where it was before, not in the row
    if (m_hold.key && m_hold.tileAt >= 0) {
        m_hold.tileAt = -1;
        if (const auto b = m_tiling.before.find(m_hold.key); b != m_tiling.before.end())
            m_hold.before = b->second;
        else if (!m_tiling.stay.contains(m_hold.key))
            m_hold.hadBefore = false; // the wall
        m_hold.before.tiled = false;
    }
    m_tiling.on = false;
    forgetTiles();
    if (m_mode == MODE_ACTIVE)
        notify("tiling off: the windows go back where they were");
    log("tiling off");
    return "not tiling";
}

// Shift+T: the ring round you, its middle where you look; not tiling: turns it on
std::string CDesktop3D::tileHere() {
    if (!m_tiling.on)
        return setTiling(true);
    if (m_mode != MODE_ACTIVE)
        return "error: not in 3D";
    anchorRing();
    return "tiling here";
}

// Y: the ring follows you or stays; not tiling, sets what T does
std::string CDesktop3D::setTileFollow(bool on) {
    auto& t = m_tiling;
    if (on == t.follow)
        return on ? "following" : "staying";
    t.follow = on;
    if (t.on && on && !t.anchorLater && (m_mode == MODE_ACTIVE || m_mode == MODE_ENTERING))
        anchorRing(false);
    if (m_mode == MODE_ACTIVE)
        notify(t.on ? (on ? "tiling: the row goes with you again" : "tiling: the row stays here. Y takes it with you again, Shift+T brings it round you")
                    : (on ? "tiling mode's row will go with you (T)" : "tiling mode's row will stay where you turn it on (T)"));
    log(on ? "tiling: following" : "tiling: staying");
    return on ? "following" : "staying";
}

// hyprwalk:tile dispatcher and hl.plugin.hyprwalk.tile(): none = T, here = Shift+T, follow = Y
std::string CDesktop3D::tileDispatch(const std::string& arg) {
    const std::string a = unquote(arg);
    if (a == "here")
        return tileHere();
    if (a == "follow")
        return setTileFollow(!m_tiling.follow);
    return setTiling(!m_tiling.on);
}

// plugin:hyprwalk:tiling and tiling_follow: applied at start and when they change
void CDesktop3D::checkTilingConfig() {
    if (const int f = !g_cfgTilingFollow || g_cfgTilingFollow->value(); f != m_tiling.configuredFollow) {
        const bool first          = m_tiling.configuredFollow < 0;
        m_tiling.configuredFollow = f;
        if (first)
            m_tiling.follow = f;
        else
            setTileFollow(f);
    }
    const int v = g_cfgTiling && g_cfgTiling->value();
    if (v == m_tiling.configured)
        return;
    const bool first    = m_tiling.configured < 0;
    m_tiling.configured = v;
    if (!first || v)
        setTiling(v);
}

void CDesktop3D::forgetTiles() {
    m_tiling.order.clear();
    m_tiling.before.clear();
    m_tiling.stay.clear();
    m_tiling.walled.clear();
    m_tiling.kept.clear();
    m_tiling.laid.clear();
    m_tiling.holdSlot    = -1;
    m_tiling.anchorLater = m_tiling.on;
}

// a toplevel not under TILE_MIN_PX (a status pill, a splash); one with no size yet counts
bool CDesktop3D::tileable(const PHLWINDOW& w) const {
    if (!toplevel(w))
        return false;
    const Vector2D size = hypr::realSize(w)->goal();
    return !((size.x >= 1 && size.x < TILE_MIN_PX) || (size.y >= 1 && size.y < TILE_MIN_PX));
}

// the window played here in the row while you're at the ring, or the held-back game's; null: none
PHLWINDOW CDesktop3D::playedInRow() const {
    if (!m_tiling.on || m_play.fill || !atRing())
        return nullptr;
    const auto w = m_play.on ? m_play.window.lock() : m_held.on ? m_held.window.lock() : nullptr;
    return w && std::ranges::contains(m_tiling.order, reinterpret_cast<uintptr_t>(w.get())) ? w : nullptr;
}

// each window's size; the window played here takes m_play.size of the view
std::vector<STileIn> CDesktop3D::tileSizes(const std::vector<uintptr_t>& keys) const {
    std::vector<STileIn> in;
    in.reserve(keys.size());
    const uintptr_t played = reinterpret_cast<uintptr_t>(playedInRow().get());
    for (const uintptr_t key : keys) {
        const auto     it   = m_placements.find(key);
        const auto     w    = it != m_placements.end() ? it->second.window.lock() : nullptr;
        const Vector2D size = w ? hypr::realSize(w)->goal() : Vector2D{};
        in.push_back({(float)size.x, (float)size.y, played && key == played ? m_play.size : 0.f});
    }
    return in;
}

// centres the ring on you; `look` also turns the row's middle to where you look
void CDesktop3D::anchorRing(bool look) {
    auto&    t = m_tiling;
    const V3 s = m_body.seen();
    if (look)
        t.ring.yaw = m_yaw;
    t.floor.reset(s);
    fitRing();
    t.ring.center = {s.x, t.floor.y + ringHeight(), s.z};
    t.ring.ground = t.floor.y;
    t.moved       = {};
    t.laid.clear();
    t.anchorLater = false;
}

// following: the ring moves with you, keeping its yaw, on the floor (STileFloor ignores jumps)
void CDesktop3D::followRing(float dt) {
    auto&    t   = m_tiling;
    const V3 s   = m_body.seen();
    const V3 was = t.ring.center;
    t.floor.step(s, m_body.onGround, m_fly, dt);
    fitRing();
    t.ring.center = {s.x, t.floor.y + ringHeight(), s.z};
    t.ring.ground = t.floor.y;
    t.moved       = t.ring.center - was;
}

void CDesktop3D::fitRing() {
    auto&      r     = m_tiling.ring;
    const auto mon   = m_monitor.lock();
    const bool third = m_thirdPerson && m_avatar;
    r.tanHalfFov     = std::tan(FOV_Y * 0.5f);
    r.aspect         = mon && mon->m_size.y > 0 ? (float)(mon->m_size.x / mon->m_size.y) : 16.f / 9.f;
    r.monitorH       = (float)std::max(m_screen.logicalSize.y, 1.0);
    r.back           = third ? m_camDist : 0.f;
    r.radius         = third ? std::max(TILE_RADIUS, m_camDist + TILE_PAST) : TILE_RADIUS;
    r.fit            = third ? THIRD_FIT : FRONT_FIT;
}

// standing height: the ring doesn't drop when you crouch
float CDesktop3D::ringHeight() const {
    return m_thirdPerson && m_avatar ? std::max(0.5f, m_avatar->height * 0.95f + 0.15f) : fpBody() ? m_avatar->eyeHeight : EYE;
}

// at the ring, windows join the row: always when following, else with your eye inside the ring
bool CDesktop3D::atRing() const {
    if (!m_tiling.on)
        return false;
    if (m_tiling.follow || m_tiling.anchorLater)
        return true;
    const V3 s = m_body.seen();
    return insideRing(m_tiling.ring, {s.x, s.y + ringHeight(), s.z});
}

// takes a window out of the row for good: tiling ending leaves it where it is
void CDesktop3D::keepFromRow(uintptr_t key) {
    std::erase(m_tiling.order, key);
    m_tiling.laid.erase(key);
    m_tiling.kept.insert(key);
    m_tiling.stay.insert(key);
    m_tiling.before.erase(key);
    m_tiling.walled.erase(key);
    if (const auto it = m_placements.find(key); it != m_placements.end())
        it->second.tiled = false;
}

// adds world and desktop-wall windows to the row in their order round you
void CDesktop3D::gatherTiles() {
    const auto&                              r = m_tiling.ring;
    std::vector<std::pair<float, uintptr_t>> round; // yaw from where you look, key
    for (auto& [key, pl] : m_placements) {
        if (pl.tiled || pl.pinned || pl.returning || pl.follows || key == m_hold.key || m_tiling.kept.contains(key) || !tileable(pl.window.lock()))
            continue;
        m_tiling.before[key] = pl;
        round.emplace_back(wrapAngle(ringYaw(r, pl.center) - r.yaw), key);
    }
    for (const auto& p : m_panels) { // panel poses come later in the frame
        const auto w = p.kind == PANEL_WINDOW ? p.window.lock() : nullptr;
        if (!w || m_placements.contains(p.key) || m_tiling.walled.contains(p.key) || !tileable(w))
            continue;
        SPlacement& pl = m_placements[p.key];
        pl             = layoutPlacement(p);
        pl.window      = w;
        round.emplace_back(wrapAngle(ringYaw(r, pl.center) - r.yaw), p.key);
    }
    std::ranges::sort(round);
    const bool fresh = m_tiling.order.empty();
    for (const auto& [yaw, key] : round) {
        m_placements[key].tiled = true;
        if (fresh)
            m_tiling.order.push_back(key);
        else
            m_tiling.order.insert(m_tiling.order.begin() + ringSlot(r, layoutRing(r, tileSizes(m_tiling.order)), r.yaw + yaw), key);
    }
}

// where you look round the ring, as a yaw from its centre (outside, facing away: your side)
float CDesktop3D::ringLookYaw() const {
    return ringLookYaw(m_camera.eye, m_camera.yaw);
}

float CDesktop3D::ringLookYaw(const V3& eye, float yaw) const {
    V3 at;
    return ringYaw(m_tiling.ring, ringLook(m_tiling.ring, eye, yaw, 0.f, at) ? at : eye);
}

int CDesktop3D::lookSlot() const {
    return ringSlot(m_tiling.ring, layoutRing(m_tiling.ring, tileSizes(m_tiling.order)), ringLookYaw());
}

// puts w into the row at `slot`, from where it's drawn now, else from the ring where you look
void CDesktop3D::tileWindow(const PHLWINDOW& w, int slot) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(w.get());
    std::erase(m_tiling.order, key);
    m_tiling.order.insert(m_tiling.order.begin() + std::clamp(slot, 0, (int)m_tiling.order.size()), key);
    m_tiling.walled.erase(key);
    m_tiling.kept.erase(key);
    auto it = m_placements.find(key);
    if (it == m_placements.end()) {
        SPlacement pl;
        if (const int i = windowPanel(w); i >= 0) // on the wall
            pl = layoutPlacement(m_panels[i]);
        else {
            const auto&    r    = m_tiling.ring;
            const Vector2D size = hypr::realSize(w)->goal();
            const auto     one  = layoutRing(r, {{(float)size.x, (float)size.y}});
            const float    at   = ringLookYaw();
            const V3       dir{std::sin(at), 0, -std::cos(at)};
            pl.center   = r.center + dir * r.radius;
            pl.center.y = one[0].center.y;
            pl.rot      = Quat::fromBasis(cross(V3{0, 1, 0}, -dir), {0, 1, 0}, -dir);
            pl.scale    = one[0].scale;
        }
        pl.window       = w;
        pl.targetCenter = pl.center;
        pl.targetRot    = pl.rot;
        pl.targetScale  = pl.scale;
        it              = m_placements.insert_or_assign(key, pl).first;
    }
    it->second.returning = false;
    it->second.pinned    = 0;
    it->second.follows   = 0;
    it->second.tiled     = true;
}

// tiling each frame: moves the ring, updates the row, lays it out and pulls windows in past obstacles
void CDesktop3D::updateTiling(float dt) {
    m_tiling.holdSlot       = -1;
    m_tiling.moved          = {};
    m_tiling.ring.lookY     = NAN;
    m_tiling.ring.lookDist  = NAN;
    m_tiling.ring.lookPitch = 0.f;
    const bool centre       = std::exchange(m_play.centre, false); // requested since the last frame
    if (!m_tiling.on || (m_mode != MODE_ACTIVE && m_mode != MODE_ENTERING))
        return;
    bool snap = false; // entering 3D: windows start in place
    if (m_tiling.anchorLater) {
        snap = m_mode == MODE_ENTERING;
        anchorRing();
        gatherTiles();
    } else if (m_tiling.follow)
        followRing(dt);
    auto&       r     = m_tiling.ring;
    auto&       order = m_tiling.order;
    std::erase_if(order, [this](uintptr_t key) {
        const auto it = m_placements.find(key);
        if (it == m_placements.end())
            return true;
        SPlacement& pl  = it->second;
        const auto  w   = pl.window.lock();
        const bool  own = toplevel(w);
        if (pl.tiled && !pl.pinned && !pl.returning && key != m_hold.key && own)
            return false;
        pl.tiled = false;
        if (w && w->m_isMapped && !own && !pl.pinned && key != m_hold.key)
            pl.returning = true; // hidden (a group's tab): home until shown
        m_tiling.laid.erase(key);
        return true;
    });
    for (const auto& p : m_panels) {
        const auto w = p.kind == PANEL_WINDOW ? p.window.lock() : nullptr;
        if (!w || m_placements.contains(p.key) || m_tiling.walled.contains(p.key) || !tileable(w))
            continue;
        const SPlacement home = layoutPlacement(p); // panel poses come later in the frame
        const int        slot = ringSlot(r, layoutRing(r, tileSizes(order)), ringYaw(r, home.center));
        SPlacement&      pl   = m_placements[p.key];
        pl                    = home;
        pl.window             = w;
        pl.tiled              = true;
        order.insert(order.begin() + slot, p.key);
    }

    std::vector<STileIn> in = tileSizes(order);
    if (const auto held = m_hold.key ? m_placements.find(m_hold.key) : m_placements.end(); held != m_placements.end() && !m_hold.onWall && atRing()) {
        if (const auto w = held->second.window.lock(); tileable(w)) {
            m_tiling.holdSlot   = ringSlot(r, layoutRing(r, in), ringYaw(r, held->second.center));
            const Vector2D size = hypr::realSize(w)->goal();
            in.insert(in.begin() + m_tiling.holdSlot, STileIn{(float)size.x, (float)size.y});
        }
    }
    // the window played here stays where you look, sized for its distance and pitch; the row turns round it
    const auto shown  = playedInRow();
    const auto played = shown ? std::ranges::find(order, reinterpret_cast<uintptr_t>(shown.get())) : order.end();
    auto&      look   = m_tiling.look;
    if (played == order.end())
        look.on = false;
    else {
        size_t i = played - order.begin();
        if (m_tiling.holdSlot >= 0 && (size_t)m_tiling.holdSlot <= i)
            ++i; // room for the carried window
        if (look.on) { // as last laid out
            r.lookY     = r.center.y + look.dy;
            r.lookDist  = look.dist;
            r.lookPitch = look.pitch;
            if (look.order != order && !centre) // row changed: turn round it
                r.yaw = wrapAngle(r.yaw + wrapAngle(look.angle - layoutRing(r, in)[i].angle));
        }
        if (m_play.on) {
            const V3   eye = m_thirdPerson && m_avatar ? camPivot() - forwardFrom(m_yaw, m_pitch) * m_camBoom : playerCamera().eye;
            V3         at;
            const bool crosses = ringLook(r, eye, m_yaw, m_pitch, at);
            r.lookY            = crosses ? at.y : NAN;
            const float mid    = crosses ? at.y : r.center.y;
            if (centre && crosses) {
                // centred: where you look, at that distance and pitch
                const float d = std::hypot(at.x - eye.x, at.z - eye.z);
                look          = {true, mid - r.center.y, d, std::atan2(mid - eye.y, std::max(d, 0.01f)), look.angle, look.order};
            } else if (look.on) {
                // update only past 2%, not every frame while the boom eases (that lays out every window again)
                const STileOut o = layoutRing(r, in)[i];
                const float    d = std::hypot(o.center.x - eye.x, o.center.z - eye.z), p = std::atan2(mid - eye.y, std::max(d, 0.01f));
                if (std::abs(d - look.dist) > look.dist * 0.02f || std::abs(p - look.pitch) > 0.02f) {
                    look.dist  = d;
                    look.pitch = p;
                }
                look.dy = mid - r.center.y;
            } else // tiling turned on while playing
                look = {true, mid - r.center.y, r.back + r.radius, std::atan2(mid - eye.y, r.back + r.radius), 0.f, order};
            r.lookDist  = look.dist;
            r.lookPitch = look.pitch;
            if (centre)
                r.yaw = wrapAngle(r.yaw + wrapAngle(ringLookYaw(eye, m_yaw) - layoutRing(r, in)[i].angle));
        }
        if (look.on) {
            look.angle = layoutRing(r, in)[i].angle;
            look.order = order;
        }
    }
    const auto laid = layoutRing(r, in);
    std::vector<std::pair<uintptr_t, size_t>> row; // window, layout index
    for (size_t i = 0, k = 0; i < laid.size() && k < order.size(); ++i)
        if ((int)i != m_tiling.holdSlot)
            row.emplace_back(order[k++], i);
    // pulled towards the ring's centre; clearance() is 153 rays: redone on slot changes, at most TILE_REPULLS a frame
    const auto pullFor = [&](size_t i) {
        if (in[i].fit > 0.f)
            return 1.f;
        const STileOut& o = laid[i];
        return std::max(clearance(r.center, o.center, o.right, o.up, o.normal, in[i].w * o.scale * 0.5f, in[i].h * o.scale * 0.5f), std::min(1.f, 0.4f / r.radius));
    };
    std::vector<std::pair<float, size_t>> went; // ring travel since, row index
    for (size_t j = 0; j < row.size(); ++j) {
        const auto [key, i] = row[j];
        const STileOut& o    = laid[i];
        const STileIn&  size = in[i];
        auto&           L    = m_tiling.laid[key];
        if (L.w != size.w || L.h != size.h || std::abs(wrapAngle(L.angle - o.angle)) > 1e-3f || std::abs(L.scale - o.scale) > o.scale * 1e-3f || L.played != (size.fit > 0.f))
            L = {o.angle, o.scale, size.w, size.h, pullFor(i), r.center, size.fit > 0.f};
        else if (const float d = length(r.center - L.at); d > TILE_REPULL)
            went.emplace_back(d, j);
    }
    std::ranges::sort(went, std::greater{});
    for (size_t n = 0; n < went.size() && n < (size_t)TILE_REPULLS; ++n) {
        const auto [key, i] = row[went[n].second];
        auto& L             = m_tiling.laid[key];
        L.pull              = pullFor(i);
        L.at                = r.center;
    }
    for (const auto& [key, i] : row) {
        SPlacement&     pl   = m_placements[key];
        const STileOut& o    = laid[i];
        const float     pull = m_tiling.laid[key].pull;
        pl.targetCenter      = r.center + (o.center - r.center) * pull;
        pl.targetRot         = Quat::fromBasis(o.right, o.up, o.normal);
        pl.targetScale       = o.scale * pull;
        if (snap) {
            pl.center = pl.targetCenter;
            pl.rot    = pl.targetRot;
            pl.scale  = pl.targetScale;
        }
    }
}

// tiling state; holdSlot -1 = none, played null = none
std::string CDesktop3D::tilingStatus() const {
    const auto& r = m_tiling.ring;
    std::string row;
    for (const uintptr_t key : m_tiling.order)
        row += std::format(R"({}"0x{:x}")", row.empty() ? "" : ", ", key);
    std::string played = "null";
    if (const float share = playedShare(); share > 0.f)
        played = std::format(R"({{"address": "0x{:x}", "fit": {:.4f}, "share": {:.4f}}})", reinterpret_cast<uintptr_t>(m_play.window.lock().get()), m_play.size, share);
    return std::format(R"({{"on": {}, "follow": {}, "atRing": {}, "center": [{:.3f}, {:.3f}, {:.3f}], "ground": {:.3f}, "yaw": {:.2f}, "radius": {:.3f}, "back": {:.3f}, "fit": {:.2f}, "row": [{}], "holdSlot": {}, "played": {}}})",
                       m_tiling.on, m_tiling.follow, atRing(), r.center.x, r.center.y, r.center.z, r.ground, r.yaw * 180.f / F_PI, r.radius, r.back, r.fit, row, m_tiling.holdSlot,
                       played);
}

PHLWINDOW CDesktop3D::findWindow(const std::string& what) const {
    if (what.starts_with("0x")) {
        const uintptr_t addr = std::strtoull(what.c_str() + 2, nullptr, 16);
        for (const auto& w : hypr::windows())
            if (w && reinterpret_cast<uintptr_t>(w.get()) == addr)
                return w;
        return nullptr;
    }
    const std::string lw = lowered(what);
    for (const bool byTitle : {false, true})
        for (const auto& w : hypr::windows())
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
            if (atRing() && tileable(w)) {
                std::erase(m_tiling.order, key);
                if (!m_tiling.before.contains(key))
                    m_tiling.stay.insert(key);
                tileWindow(w, lookSlot());
                return "here";
            }
            if (!placeInFront(w, appRule(m_rules, classOf(w))))
                return "error: it has no size yet";
            if (m_tiling.on && tileable(w)) // away from a staying ring: it stays here
                keepFromRow(key);
            return "here";
        case WA_WALL:
            forgetSpot(key);
            returnToWall(key);
            return "on the wall";
        case WA_PIN: return setPinned(w, placed == m_placements.end() || !placed->second.pinned);
        case WA_BIGGER:
        case WA_SMALLER: return resizeReal(w, hypr::realSize(w)->goal() * (a == WA_BIGGER ? 1.25 : 0.8));
        case WA_PLAY: {
            if (m_mode != MODE_ACTIVE)
                return "error: not in 3D";
            if (windowPanel(w) < 0 && placed == m_placements.end()) { // from another workspace: bring it first
                if (atRing() && tileable(w)) {
                    m_tiling.stay.insert(key);
                    tileWindow(w, lookSlot());
                } else if (placeInFront(w, appRule(m_rules, classOf(w))) && m_tiling.on && tileable(w)) {
                    // away from a staying ring: as without tiling, and it stays put when tiling ends
                    m_tiling.stay.insert(key);
                }
            }
            const int i = windowPanel(w);
            if (i < 0) { // shows from the next frame
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

// pinned: follows the view, top right, stacked down; unpinned: stays where it is
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
        // stays where you see it, pulled nearer and smaller out of any wall it was drawn over
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
    if (it == m_placements.end() || it->second.returning) {
        // from where it's drawn now (e.g. the wall)
        const int i = windowPanel(w);
        if (i < 0)
            return "error: it isn't in the 3D view";
        const SPanel& p  = m_panels[i];
        SPlacement&   pl = m_placements[key];
        pl.window        = w;
        pl.center = pl.targetCenter = p.pose.at(p.box.size() * 0.5);
        pl.rot = pl.targetRot = Quat::fromBasis(p.pose.right, -p.pose.down, p.pose.normal);
        pl.scale = pl.targetScale = p.pose.scale;
        pl.returning     = false;
        it               = m_placements.find(key);
    }
    auto& pl = it->second;
    if (!pl.pinned) { // size and distance to take it back with
        const bool held    = m_hold.key == key;
        pl.pinnedFromScale = held ? m_screen.scale() * m_hold.scaleMul : pl.targetScale;
        pl.pinnedFromDist  = held ? m_hold.dist : length(pl.targetCenter - m_camera.eye) - carryFrom();
    }
    if (m_hold.key == key)
        m_hold = {};
    pl.pinned  = ++m_pinCount;
    pl.tiled   = false;
    pl.follows = 0;     // a dialog no longer follows its window
    return "pinned";
}

// Shift+H: pin the carried window, else take back the last pinned (unreachable by aim), else pin the aimed one
std::string CDesktop3D::togglePin() {
    if (m_mode != MODE_ACTIVE)
        return "error: not in 3D";
    PHLWINDOW w;
    if (m_hold.key)
        if (const auto it = m_placements.find(m_hold.key); it != m_placements.end())
            w = it->second.window.lock();
    if (w)
        return setPinned(w, true);

    if (m_menu.open()) // the crosshair is hidden
        return "error: close the Action Menu first";
    uint64_t  last = 0;
    uintptr_t lastKey = 0;
    for (const auto& [key, pl] : m_placements) {
        const auto pw = pl.window.lock();
        if (pl.pinned > last && pw && windowPanel(pw) >= 0) { // drawn only, not a group's hidden tab
            last    = pl.pinned;
            lastKey = key;
        }
    }
    if (lastKey) {
        takePinned(lastKey);
        return m_hold.key == lastKey ? "holding" : "error: it can't be taken";
    }

    if (m_aimed >= 0 && m_aimed < (int)m_panels.size() && m_panels[m_aimed].kind != PANEL_LAYER)
        w = m_panels[m_aimed].window.lock();
    if (!w)
        return "error: point the crosshair at a window to pin it";
    return setPinned(w, true);
}

// G and H: put down the carried window where the crosshair points, else pick one up
std::string CDesktop3D::carry() {
    if (m_mode != MODE_ACTIVE)
        return "error: not in 3D";
    if (m_hold.key) {
        place();
        return "placed";
    }
    if (m_menu.open()) // the crosshair is hidden
        return "error: close the Action Menu first";
    grab();
    return m_hold.key ? "holding" : "error: point the crosshair at a window to pick it up";
}

// resizes the real window (logical px), floating it first: only floating windows take any size
std::string CDesktop3D::resizeReal(const PHLWINDOW& w, const Vector2D& size) {
    if (hypr::fullscreenOrMaximized(w))
        return "error: it's fullscreen";
    const Vector2D want{std::round(std::clamp(size.x, 64.0, 8192.0)), std::round(std::clamp(size.y, 48.0, 8192.0))};
    if (!w->m_isFloating)
        (void)Config::Actions::floatWindow(Config::Actions::TOGGLE_ACTION_ENABLE, w); // resize says if it can't
    if (const auto r = Config::Actions::resize(want, false, w); !r)
        return "error: " + r.error().message;
    return std::format("{:.0f}x{:.0f}", want.x, want.y);
}

// the Action Menu's Apps (the favourites, then all) and Windows pages, and a window's own
SMenuPage CDesktop3D::ownPage(const std::string& id) {
    SMenuPage p;
    // icons load a few a frame in update() and show once loaded
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
        p.items.push_back({.label  = "Tiling",
                           .hint   = m_tiling.on ? std::format("on: {} round you", m_tiling.order.size()) : "off",
                           .icon   = "🔲",
                           .action = MA_TILING,
                           .on     = m_tiling.on});
        if (m_tiling.on)
            p.items.push_back({.label  = "Follow me",
                               .hint   = m_tiling.follow ? "going with you (Y)" : "staying here (Y)",
                               .icon   = "👣",
                               .action = MA_TILING_FOLLOW,
                               .on     = m_tiling.follow});
        for (const auto& w : hypr::windows()) {
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
    if (id == "maps") {
        p.title                    = "Maps";
        const bool        loading  = m_mapLoader.busy();
        const std::string shown    = normalPath(m_world.model ? m_world.model->path : ""), next = normalPath(m_mapPath),
                          defaults = normalPath(configuredMap());
        const auto        hint     = [&](const std::string& path, std::string other) {
            std::string h = loading && path == next ? "loading…" : path == shown ? "here" : std::move(other);
            return path == defaults ? h + " · default" : h;
        };
        for (const auto& f : mapFiles())
            p.items.push_back({.label = clipped(f.label, 28), .hint = hint(f.path, fileSize(f.bytes)), .icon = "🗺️", .action = MA_MAP, .target = f.path, .on = f.path == next});
        p.items.push_back({.label = "Courtyard", .hint = hint("", "built in"), .icon = "🏛️", .action = MA_MAP, .target = "", .on = next.empty()});
        return p;
    }
    if (id == "avatars") {
        p.title                    = "Avatars";
        const bool        loading  = m_avatarLoader.busy();
        const std::string shown    = normalPath(m_avatar ? m_avatar->path : ""), next = normalPath(m_avatarPath),
                          defaults = normalPath(configuredPath(g_cfgAvatar));
        for (const auto& f : avatarFiles()) {
            std::string hint = loading && f.path == next ? "loading…" : f.path == shown ? "here" : fileSize(f.bytes);
            if (f.path == defaults)
                hint += " · default";
            p.items.push_back({.label = clipped(f.label, 28), .hint = hint, .icon = "🧍", .action = MA_AVATAR, .target = f.path, .on = f.path == next});
        }
        return p;
    }
    // close:ADDRESS: "Close it" only in slot 5, so repeating the key or click that picked Close… (8) keeps it
    if (id.starts_with("close:")) {
        const std::string addr = "0x" + id.substr(6);
        const auto        w    = findWindow(addr);
        if (!w)
            return {.title = "Gone"};
        p.title = "Close " + clipped(w->m_title.empty() ? classOf(w) : w->m_title, 24) + "?";
        for (int slot = 1; slot <= CActionMenu::SLOTS; ++slot)
            p.items.push_back(slot == 5 ? SMenuItem{.label = "Close it", .icon = "❌", .action = MA_WINDOW, .arg = WA_CLOSE, .target = addr}
                                        : SMenuItem{.label = "Keep it", .icon = "↩️", .action = MA_MENU_BACK});
        return p;
    }
    // win:ADDRESS: what to do with it
    const std::string addr = "0x" + id.substr(4);
    const auto        w    = findWindow(addr);
    if (!w)
        return {.title = "Gone"};
    const auto pl     = m_placements.find(reinterpret_cast<uintptr_t>(w.get()));
    const bool placed = pl != m_placements.end() && !pl->second.returning, pinned = placed && pl->second.pinned;
    const auto size   = hypr::realSize(w)->goal();
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
        {.label = "Close…", .hint = "asks first", .icon = "❌", .page = std::format("close:{:x}", reinterpret_cast<uintptr_t>(w.get()))},
    };
    return p;
}

std::string CDesktop3D::windowsStatus() const {
    std::string list;
    for (const auto& [key, pl] : m_placements) {
        const auto w = pl.window.lock();
        // height in metres; apparent: size seen from the eye, 1 = as on the 2D desktop; follows: a dialog's window
        const V3 n = pl.rot.rotate({0, 0, 1});
        list += std::format(R"({}{{"class": "{}", "title": "{}", "address": "0x{:x}", "center": [{:.3f}, {:.3f}, {:.3f}], "normal": [{:.3f}, {:.3f}, {:.3f}], "distance": {:.3f}, "size": {:.3f}, "height": {:.3f}, "width": {:.3f}, "apparent": {:.3f}, "held": {}, "pinned": {}, "tiled": {}, "follows": "{}", "settled": {}, "returning": {}}})",
                            list.empty() ? "" : ", ", jsonEscape(w ? w->m_class : ""), jsonEscape(w ? w->m_title : ""), key, pl.center.x, pl.center.y, pl.center.z, n.x, n.y, n.z,
                            length(pl.center - m_camera.eye), pl.scale / m_screen.scale(), w ? hypr::realSize(w)->goal().y * pl.scale : 0.0,
                            w ? hypr::realSize(w)->goal().x * pl.scale : 0.0, apparentSize(pl.center, pl.scale), key == m_hold.key, pl.pinned != 0, pl.tiled,
                            pl.follows ? std::format("0x{:x}", pl.follows) : "", pl.settled, pl.returning);
    }
    const std::string hold = m_hold.key ? std::format(R"({{"dist": {:.3f}, "size": {:.3f}, "onWall": {}}})", m_hold.dist, m_hold.scaleMul, m_hold.onWall) : "null";
    return std::format(R"({{"placed": [{}], "hold": {}, "spots": {}, "tiling": {}}})", list, hold, m_spots.size(), tilingStatus());
}

// ----------------------------------------------------------------- drawing

namespace {
    // how far down the top right Hyprland's error bar and notifications reach (focused monitor, output px); NOTIF_*
    // copied from NotificationOverlay.cpp
    float hyprlandOverlaysBottom(const PHLMONITOR& mon) {
        if (!mon || mon != Desktop::focusState()->monitor())
            return 0;
        constexpr float OFFSET_Y = 10, PAD_Y = 10, GAP_Y = 10;
        static auto     PERRORPOS = CConfigValue<Config::INTEGER>("debug:error_position");
        float           bottom    = 0;
        if (ErrorOverlay::overlay()->active() && *PERRORPOS == 0)
            bottom = 10.f * (float)mon->m_scale + ErrorOverlay::overlay()->height(); // pad: 10 logical px
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
    f.aimed     = m_mode == MODE_ACTIVE && !m_menu.visible() && !m_play.on && m_play.t <= 0.f && !m_shell.input ? m_aimed : -1;
    // no crosshair in play mode or away; only its dot where the app's cursor shows
    f.crosshair    = !m_menu.visible() && !m_play.on && m_play.t <= 0.f && !m_away && !m_shell.input;
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
        // first person shows the headless body only; an emote's camera behind shows it whole
        const bool body      = fpBody() && m_mode == MODE_ACTIVE && !(m_play.t > 0.f && m_play.framed) && !m_fpCramped;
        const bool out       = m_emoteView > 0.f && length(m_camera.eye - playerCamera().eye) > 0.3f;
        f.avatar.firstPerson = body && !out;
        f.avatar.visible     = (m_thirdPerson && (m_mode != MODE_ACTIVE || m_camBoom > 0.35f)) || body;
        f.avatar.sky     = m_avatarLight.skyAvg;
        f.avatar.bounce  = m_avatarLight.bounceAvg;
    }

    f.menu = m_menu.hud();
    f.menu.alpha *= f.hudAlpha;
    m_badgeBox = {};
    if (m_mic.on()) {
        // lip sync badge (and microphone problems): top right, under Hyprland's notifications
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
    if (m_mic.on()) { // restart on the new source
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
    // microphone problems the badge reports
    enum eMicProblem : uint8_t {
        MP_STARTING, // nothing known yet
        MP_NONE,
        MP_ERROR,    // PipeWire ended the stream
        MP_UNLINKED, // nothing feeds it
        MP_MUTED,    // PipeWire has the source muted
        MP_SILENT,   // exact zeros: muted on the device itself
        MP_NOTHING,  // no samples at all
        MP_MISSING,  // lipsync_source missing: using the default
    };

    bool micThere(const SMicStatus& s) { // the one asked for (none: the default)
        return s.target.empty() || std::ranges::any_of(s.sources, [&](const auto& src) { return src.first == s.target; });
    }

    std::string micName(const SMicStatus& s) {
        std::string n = !s.sourceNick.empty() ? s.sourceNick : !s.sourceDescription.empty() ? s.sourceDescription : s.sourceName;
        if (n.size() > 28) { // whole UTF-8 characters
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
    if (m_micWatch.problem >= 0 && dt < 0.25f)
        return;
    m_micWatch.looked = now;
    SMicStatus st     = m_mic.status();
    // broken stream: reopen every 3 s, so a microphone plugged in brings lip sync back
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

// ------------------------------------------------------------ action menu

std::string CDesktop3D::menuAction(const SMenuItem& it) {
    switch (it.action) {
        case MA_VIEW: return setView(!m_thirdPerson);
        case MA_FLY:
            m_fly = !m_fly;
            m_body.vel = {};
            return m_fly ? "flying" : "walking";
        case MA_RESPAWN: resetPlayer(); return "ok";
        case MA_LIPSYNC: return setLipSync(!m_lipsync);
        case MA_LAUNCH: {
            const std::string r = launch(it.target);
            if (!r.starts_with("error: "))
                m_menu.hide();
            return r;
        }
        case MA_WINDOW: {
            const auto a = (eWindowAction)it.arg;
            const auto w = findWindow(it.target);
            static constexpr const char* ACTIONS[] = {"focus", "bring here", "to the wall", "pin", "bigger", "smaller", "play", "close"};
            if (w)
                logf("the Action Menu: {} {} ({})", a == WA_PIN && it.on ? "unpin" : a < std::size(ACTIONS) ? ACTIONS[a] : "?", classOf(w), clipped(w->m_title, 40));
            const std::string r = windowAction(w, a);
            if (!r.starts_with("error: ") && (a == WA_FOCUS || a == WA_BRING || a == WA_PLAY || a == WA_CLOSE))
                m_menu.hide();
            return r;
        }
        case MA_MENU_BACK: m_menu.back(); return "ok"; // Keep it on the Close page
        case MA_MAP: {
            const std::string r = pickMap(it.target);
            if (!r.starts_with("error: "))
                m_menu.hide();
            return r;
        }
        case MA_AVATAR: {
            const std::string r = pickAvatar(it.target);
            if (!r.starts_with("error: "))
                m_menu.hide();
            return r;
        }
        case MA_TILING: {
            const std::string r = setTiling(!m_tiling.on);
            if (!r.starts_with("error: "))
                m_menu.hide();
            return r;
        }
        case MA_TILING_FOLLOW: {
            const std::string r = setTileFollow(!m_tiling.follow);
            if (!r.starts_with("error: "))
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
        if (m_shell.input)
            return "error: a layer surface over the view has the keyboard: close it first";
        if (m_away)
            setAway(false); // the menu needs the mouse and keys
        setTyping(false);
    }
    return hyprwalk::menuCommand(m_menu, args, [this](const SMenuItem& it) { return menuAction(it); });
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
    std::string shell = "null";
    if (const auto ls = m_shell.layer.lock())
        shell = std::format(R"({{"namespace": "{}", "input": {}, "pointer": [{:.1f}, {:.1f}], "over": {}}})", jsonEscape(ls->m_namespace), m_shell.input, m_shell.pointer.x,
                            m_shell.pointer.y, m_shell.input && g_pSeatManager->m_state.pointerFocus ? "true" : "false");
    std::string cursor = "null";
    if (m_cursorShown) {
        const auto size = hypr::pointer()->cursorSizeLogical();
        const auto hot  = hypr::pointer()->hotspot();
        cursor = std::format(R"({{"at": [{:.1f}, {:.1f}], "size": [{:.0f}, {:.0f}], "hotspot": [{:.0f}, {:.0f}]}})", m_pointerAt.local.x, m_pointerAt.local.y, size.x, size.y, hot.x, hot.y);
    }
    return std::format(
        R"({{"mode": "{}", "monitor": "{}", "away": {}, "view": "{}", "body": {}, "typing": {}, "playing": {}, "playHeld": {}, "shell": {}, "cursor": {}, "fly": {}, "feet": [{:.3f}, {:.3f}, {:.3f}], "seenY": {:.3f}, "eye": [{:.3f}, {:.3f}, {:.3f}], "yaw": {:.2f}, "pitch": {:.2f}, "onGround": {}, "panels": {}, "aimed": {}, "fps": {:.1f}, "frames": {}, "minDt": {:.5f}, "updateMs": {:.2f}, "renderMs": {:.2f}, "sens": {}, "placed": {}, "holding": {}, "tiling": {}, "world": "{}", "map": "{}", "mapLoading": {}, "avatar": "{}", "avatarLoading": {}, "anim": "{}", "exposure": {:.2f}, "light": {:.3f}, "menu": "{}", "hooks": {{"motion": {}, "warp": {}, "cursor": {}, "wheel": {}, "frame": {}, "softCursor": {}, "discard": {}}}}})",
        modes[m_mode], m_mode != MODE_OFF && m_monitor.lock() ? jsonEscape(m_monitor.lock()->m_name) : "", m_mode != MODE_OFF && m_away, m_thirdPerson ? "third" : "first", fpBody(), m_typing, playStatus(), m_mode == MODE_ACTIVE && m_held.on, shell, cursor, m_fly, m_body.feet.x, m_body.feet.y, m_body.feet.z, m_body.seen().y, m_camera.eye.x, m_camera.eye.y, m_camera.eye.z, m_yaw * 180.f / F_PI, m_pitch * 180.f / F_PI, m_body.onGround,
        m_panels.size(), aimed, m_fps, m_frames, m_minDt, m_updateMs, m_renderMs, m_sens, m_placements.size(), m_hold.key != 0, m_tiling.on, jsonEscape(m_world.name), jsonEscape(m_mapPath), m_mapLoader.busy(),
        jsonEscape(m_avatarPath), m_avatarLoader.busy(), jsonEscape(m_anim.playing()), m_exposure, m_lightAvg, m_menu.open() ? jsonEscape(m_menu.path()) : "", m_hookMoved != nullptr, m_hookWarp != nullptr, m_hookCursor != nullptr, m_hookWheel != nullptr, m_hookFrame != nullptr, m_hookSoftCursor != nullptr, m_hookDiscard != nullptr);
}

std::string CDesktop3D::hyprctl(const std::string& request) {
    std::istringstream       in(request);
    std::vector<std::string> args;
    for (std::string s; in >> s;)
        args.push_back(s);
    if (!args.empty())
        args.erase(args.begin()); // "hyprwalk"

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
    if (cmd == "on") { // on [MONITOR] (else plugin:hyprwalk:monitor's or focused)
        PHLMONITOR mon;
        if (args.size() > 1) {
            std::string name = args[1];
            for (size_t i = 2; i < args.size(); ++i) // descriptions have spaces
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
    if (cmd == "away") { // away [on|off|toggle]: input to another monitor
        const std::string v = args.size() > 1 ? args[1] : "toggle";
        if (v != "on" && v != "off" && v != "toggle")
            return "error: away [on|off|toggle]";
        return setAway(v == "on" || (v == "toggle" && !m_away));
    }
    if (cmd == "type") {
        return setTyping(args.size() > 1 ? args[1] != "off" : !m_typing);
    }
    if (cmd == "play") { // play [on|off|toggle] [here|fill]; none: status
        if (args.size() < 2)
            return playStatus();
        std::string words;
        for (size_t i = 1; i < args.size(); ++i)
            words += args[i] + " ";
        return playDispatch(words);
    }
    if (cmd == "camera") { // the drawn camera, degrees (status: the player's)
        return std::format(R"({{"eye": [{:.3f}, {:.3f}, {:.3f}], "yaw": {:.2f}, "pitch": {:.2f}, "up": [{:.4f}, {:.4f}, {:.4f}]}})", m_camera.eye.x, m_camera.eye.y,
                           m_camera.eye.z, m_camera.yaw * 180.f / F_PI, m_camera.pitch * 180.f / F_PI, m_camUp.x, m_camUp.y, m_camUp.z);
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
        m_body.feet  = {num(1, m_body.feet.x), num(2, m_body.feet.y), num(3, m_body.feet.z)};
        m_body.vel   = {};
        m_body.seenY = NAN;
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
        m_body.vel = {};
        return m_fly ? "flying" : "walking";
    }
    if (cmd == "click") { // click [left|right|middle]
        const std::string b      = args.size() > 1 ? args[1] : "left";
        const uint32_t    button = b == "right" ? BTN_RIGHT_ : b == "middle" ? BTN_MIDDLE_ : BTN_LEFT_;
        onButton(nowMs(), button, true, nullptr);
        onButton(nowMs(), button, false, nullptr);
        return "ok";
    }
    if (cmd == "aim") { // aim [window]: face it or the one nearest the crosshair
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
            if (dot(d, p.pose.normal) > -0.05f) // from behind or edge on
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
    if (cmd == "grab") // like G and H
        return carry();
    if (cmd == "place") {
        if (!m_hold.key)
            return "error: not holding anything";
        place();
        return "placed";
    }
    if (cmd == "pin") // like Shift+H
        return togglePin();
    if (cmd == "hold") { // hold <distance> [scale] of the carried window
        if (!m_hold.key)
            return "error: not holding anything";
        m_hold.dist     = std::clamp(num(1, m_hold.dist), CARRY_NEAREST, 12.f);
        m_hold.scaleMul = std::clamp(num(2, m_hold.scaleMul), 0.2f, 5.f);
        return "ok";
    }
    if (cmd == "tile") { // tile: T, Shift+T (here), Y (follow)
        const std::string v = args.size() > 1 ? args[1] : "";
        if (v.empty())
            return tilingStatus();
        if (v == "here")
            return tileHere();
        if (v == "follow") {
            const std::string f = args.size() > 2 ? args[2] : "toggle";
            if (f != "on" && f != "off" && f != "toggle")
                return "error: tile follow [on|off|toggle]";
            return setTileFollow(f == "on" || (f == "toggle" && !m_tiling.follow));
        }
        if (v != "on" && v != "off" && v != "toggle")
            return "error: tile [on|off|toggle|here|follow]";
        return setTiling(v == "on" || (v == "toggle" && !m_tiling.on));
    }
    if (cmd == "reset-windows") { // all to the wall; forget: also the spots
        m_hold      = {};
        m_tiling.on = false;
        forgetTiles();
        for (auto& [key, pl] : m_placements) {
            pl.returning = true;
            pl.pinned    = 0;
            pl.tiled     = false;
        }
        if (args.size() > 1 && args[1] == "forget") {
            loadSpots();
            m_spots.clear();
            saveWindowSpots(m_mapPath, m_spots);
        }
        return "ok";
    }
    if (cmd == "panels") { // everything drawn in 3D, in drawing order
        static constexpr const char* KINDS[] = {"layer", "window", "popup"};
        const auto                   mon    = m_monitor.lock();
        const float                  aspect = mon && mon->m_size.y > 0 ? (float)(mon->m_size.x / mon->m_size.y) : 16.f / 9.f;
        const float                  tanY   = std::tan(FOV_Y * 0.5f);
        std::string                  list;
        for (size_t i = 0; i < m_panels.size(); ++i) {
            const auto& p = m_panels[i];
            const auto  w = p.window.lock();
            std::string surfaces; // desktop coords; not the app's cursor
            for (const auto& sf : p.surfaces)
                if (sf.surface)
                    surfaces += std::format("{}[{:.0f}, {:.0f}, {:.0f}, {:.0f}]", surfaces.empty() ? "" : ", ", p.box.x + sf.box.x, p.box.y + sf.box.y, sf.box.w, sf.box.h);
            // its centre within the last drawn view
            const V3   c      = m_view.point(p.pose.at(p.box.size() * 0.5));
            const bool inView = m_mode != MODE_OFF && c.z < 0 && std::abs(c.y) <= -c.z * tanY && std::abs(c.x) <= -c.z * tanY * aspect;
            list += std::format(R"({}{{"kind": "{}", "class": "{}", "title": "{}", "box": [{:.0f}, {:.0f}, {:.0f}, {:.0f}], "placed": {}, "front": {}, "aimed": {}, "alpha": {:.2f}, "inView": {}, "surfaces": [{}]}})",
                                list.empty() ? "" : ", ", KINDS[p.kind], jsonEscape(w ? w->m_class : ""), jsonEscape(w ? w->m_title : ""), p.box.x, p.box.y, p.box.w, p.box.h,
                                p.placed, p.front, (int)i == m_aimed, p.alpha, inView, surfaces);
        }
        return "[" + list + "]";
    }
    if (cmd == "windows") // windows off the wall (size 1 = as on the wall)
        return windowsStatus();
    if (cmd == "log") // log [lines] (Hyprland logs it only with debug on)
        return logLines(args.size() > 1 ? (size_t)std::max(1, std::atoi(args[1].c_str())) : 400);
    // the raw request after `words` words, so paths can have spaces
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

    if (cmd == "launch") {
        const std::string what = unquote(afterWords(2));
        return what.empty() ? "error: launch <desktop id, name or command>" : launch(what);
    }
    if (cmd == "apps") {
        std::string list;
        for (const auto& e : apps())
            list += std::format(R"({}{{"id": "{}", "name": "{}", "exec": "{}", "class": "{}", "icon": {}}})", list.empty() ? "" : ", ", jsonEscape(e.id), jsonEscape(e.name),
                                jsonEscape(e.exec), jsonEscape(e.wmClass), appIcon(e.icon, ICON_PX) != nullptr);
        return "[" + list + "]";
    }
    if (cmd == "window") {
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
            if (sub == "forget") { // re-guess the spawn and desktop
                std::error_code ec;
                std::filesystem::remove(mapStatePath(m_mapPath), ec);
            }
            const float scale = sub == "scale" ? num(2, 0) : sub == "forget" ? (g_cfgMapScale ? g_cfgMapScale->value() : 0.f) : m_mapScale;
            return requestMap(m_mapPath, scale);
        }
        return requestMap(rest, g_cfgMapScale ? g_cfgMapScale->value() : 0.f);
    }
    // avatar [path|none|reload|height <meters>|lipsync ...]; the other subcommands are m_ctl.command's (control.cpp)
    if (cmd == "avatar") {
        const std::string rest = pathArg();
        const std::string sub  = args.size() > 1 ? args[1] : "";
        if (rest.empty())
            return avatarStatus();
        if (sub == "lipsync") {
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
        m_ctl.emoteSound    = emoteSoundStatus();
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
        if (v == "body") { // first-person body, until the config changes
            const std::string b = args.size() > 2 ? args[2] : "";
            if (b == "on" || b == "off" || b == "toggle")
                m_fpBodyOn = b == "on" || (b == "toggle" && !m_fpBodyOn);
            else if (!b.empty())
                return "error: view body [on|off|toggle]";
            return m_fpBodyOn ? "on" : "off";
        }
        const std::string r = setView(v == "third" || (v == "toggle" && !m_thirdPerson));
        if (m_thirdPerson) {
            m_camDist = std::clamp(num(2, m_camDist), 0.8f, 10.f);
            m_camSide = std::clamp(num(3, m_camSide), -1.f, 1.f);
        }
        return r;
    }
    if (cmd == "spawn") { // spawn [here]: respawn, or set the spawn here
        if (args.size() > 1 && args[1] == "here") {
            if (m_mode != MODE_ACTIVE)
                return "error: not in 3D";
            m_world.spawn    = m_body.feet;
            m_world.spawnYaw = m_yaw;
            if (!m_mapPath.empty() && m_world.model && !saveMapState(m_mapPath, m_world, m_mapScale))
                return "error: couldn't save to " + mapStatePath(m_mapPath);
            return "ok";
        }
        resetPlayer();
        return "ok";
    }
    if (cmd == "desktop") { // desktop here [height m]: at the crosshair
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
    return "usage: hyprctl hyprwalk [status|toggle|on [monitor]|off [now]|away [on|off|toggle]|type [on|off]|play [on|off|toggle] [here|fill]|camera|look dx dy|turn yaw pitch|tp x y z|walk secs [forward|back|left|right]|jump|fly|click "
           "[left|right|middle]|sens [value]|aim [window]|grab|place|hold dist [scale]|pin|tile [on|off|toggle|here|follow [on|off|toggle]]|reset-windows [forget]|windows|panels|log [lines]|launch what|apps|window sel action|map [path|none|reload|forget|scale s]|spawn [here]|desktop [here [height]]|"
           "avatar [path|none|reload|height m|expression [name [weight]|none]|gesture [left|right|both gesture]|parts [reset]|toggle name [on|off|reset]|"
           "shape name [weight|reset]|physics [on|off|toggle]|emote [name|number|file|folder [once|loop]|stop]|lipsync [on|off|toggle|gain dB|auto|source name|default]]|"
           "view [first|third|toggle] [distance] [side]|view body [on|off|toggle]|"
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
    int luaAway(lua_State*) {
        if (g_p3D)
            g_p3D->setAway(!g_p3D->away());
        return 0;
    }

    // Lua C API functions, looked up at call time: needing them at load fails where Hyprland has Lua built in
    template <typename F>
    F luaFunction(const char* name) {
        return reinterpret_cast<F>(dlsym(RTLD_DEFAULT, name));
    }

    // hl.plugin.hyprwalk.menu([args]): as hyprctl hyprwalk menu (none: toggle, a page: open it)
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

    // hl.plugin.hyprwalk.tile([here|follow]): T, Shift+T or Y; returns what hyprctl hyprwalk tile says
    int luaTile(lua_State* L) {
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
        const std::string r = g_p3D->tileDispatch(arg);
        if (!pushString)
            return 0;
        pushString(L, r.data(), r.size());
        return 1;
    }

    // hl.plugin.hyprwalk.play([on|off|toggle] [here|fill]): as hyprctl hyprwalk play
    int luaPlay(lua_State* L) {
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
        const std::string r = g_p3D->playDispatch(arg);
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
        throw std::runtime_error("[hyprwalk] version mismatch");
    }

    // plugin { hyprwalk { layer_spacing = ..., wallpaper = ... } }
    g_cfgSpacing = makeShared<Config::Values::CFloatValue>("plugin:hyprwalk:layer_spacing", "meters between stacked desktop layers in 3D", 0.02f,
                                                          Config::Values::SFloatValueOptions{.min = 0.f, .max = 0.5f});
    g_cfgWallpaper = makeShared<Config::Values::CBoolValue>("plugin:hyprwalk:wallpaper", "keep the wallpaper on the desktop wall in 3D", false);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgSpacing);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgWallpaper);
    // plugin { hyprwalk { map = "~/maps/de_dust2.glb", map_scale = 0 } }
    g_cfgMap      = makeShared<Config::Values::CStringValue>("plugin:hyprwalk:map", "glTF map to walk around in instead of the courtyard", "");
    g_cfgMapScale = makeShared<Config::Values::CFloatValue>("plugin:hyprwalk:map_scale", "meters per map unit, 0 = guess", 0.f,
                                                            Config::Values::SFloatValueOptions{.min = 0.f, .max = 1000.f});
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgMap);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgMapScale);
    // plugin { hyprwalk { avatar = "~/avatars/me.vrm", avatar_height = 0, avatar_physics = true } }
    g_cfgAvatar       = makeShared<Config::Values::CStringValue>("plugin:hyprwalk:avatar", "glTF / GLB / VRM avatar, seen in third person (V)", "");
    g_cfgAvatarHeight = makeShared<Config::Values::CFloatValue>("plugin:hyprwalk:avatar_height", "the avatar's height in meters, 0 = as it comes", 0.f,
                                                                Config::Values::SFloatValueOptions{.min = 0.f, .max = 20.f});
    g_cfgAvatarPhysics = makeShared<Config::Values::CBoolValue>("plugin:hyprwalk:avatar_physics", "the avatar's hair, skirt and the like swing as it moves", true);
    // plugin { hyprwalk { first_person_body = true } }
    g_cfgFirstPersonBody = makeShared<Config::Values::CBoolValue>("plugin:hyprwalk:first_person_body",
                                                                  "first person from the avatar's eyes: its body below, its hands in view doing what you do", true);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgFirstPersonBody);
    // plugin { hyprwalk { lipsync = false } }: nothing heard is kept or sent
    g_cfgLipSync = makeShared<Config::Values::CBoolValue>("plugin:hyprwalk:lipsync", "lip sync: the microphone moves the avatar's mouth while in 3D", false);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgLipSync);
    // plugin { hyprwalk { lipsync_gain = auto, lipsync_source = "" } }
    g_cfgLipSyncGain = makeShared<Config::Values::CStringValue>("plugin:hyprwalk:lipsync_gain",
                                                                "lip sync: how much louder the microphone counts, dB (-20 to 60), or auto: it goes by your voice", "auto");
    g_cfgLipSyncSource = makeShared<Config::Values::CStringValue>("plugin:hyprwalk:lipsync_source",
                                                                  "lip sync: the microphone, by its name or description (wpctl status), \"\" = the default one", "");
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgLipSyncGain);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgLipSyncSource);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatar);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatarHeight);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatarPhysics);
    g_cfgAvatarEmotes = makeShared<Config::Values::CStringValue>("plugin:hyprwalk:avatar_emotes",
                                                                 "more emotes: VRM animations (.vrma) or glTF clips, files or folders separated by commas", "");
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAvatarEmotes);
    // plugin { hyprwalk { emote_volume = 0.5 } }
    g_cfgEmoteVolume = makeShared<Config::Values::CFloatValue>("plugin:hyprwalk:emote_volume",
                                                               "how loud emotes' sounds (a dance's song) play: 1 as loud as they are, 0 not at all", EMOTE_VOLUME,
                                                               Config::Values::SFloatValueOptions{.min = 0.f, .max = 1.f});
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgEmoteVolume);
    // plugin { hyprwalk { apps = firefox, discord, obs; app_rules = steam_app_.*: 2.2 1.5, discord: 1.2 auto left } }
    g_cfgApps     = makeShared<Config::Values::CStringValue>("plugin:hyprwalk:apps", "the Apps page's favourites: desktop ids, names or commands, separated by commas", "");
    g_cfgAppRules = makeShared<Config::Values::CStringValue>("plugin:hyprwalk:app_rules",
                                                             "where apps launched from 3D open: CLASS: DISTANCE [HEIGHT|auto] [left|right|SIDE], separated by commas", "");
    g_cfgPinSize  = makeShared<Config::Values::CFloatValue>("plugin:hyprwalk:pin_size", "how much of the view's height a window pinned to it takes", 0.3f,
                                                           Config::Values::SFloatValueOptions{.min = 0.05f, .max = 1.f});
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgApps);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAppRules);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgPinSize);
    // plugin { hyprwalk { monitor = DP-1 } }
    g_cfgMonitor = makeShared<Config::Values::CStringValue>("plugin:hyprwalk:monitor",
                                                            "the monitor 3D goes on: its name (DP-1) or desc: and its description; \"\" = the focused one", "");
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgMonitor);
    // plugin { hyprwalk { walk_speed = 1.6, run_speed = 4.5 } }: m/s
    g_cfgWalkSpeed = makeShared<Config::Values::CFloatValue>("plugin:hyprwalk:walk_speed", "how fast you walk in 3D, m/s", WALK_SPEED,
                                                             Config::Values::SFloatValueOptions{.min = 0.3f, .max = 10.f});
    g_cfgRunSpeed  = makeShared<Config::Values::CFloatValue>("plugin:hyprwalk:run_speed", "how fast you run in 3D (Shift), m/s", RUN_SPEED,
                                                            Config::Values::SFloatValueOptions{.min = 0.5f, .max = 15.f});
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgWalkSpeed);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgRunSpeed);
    // plugin { hyprwalk { tiling = false, tiling_follow = true } }
    g_cfgTiling       = makeShared<Config::Values::CBoolValue>("plugin:hyprwalk:tiling", "tiling mode (T): the windows in 3D side by side round you", false);
    g_cfgTilingFollow = makeShared<Config::Values::CBoolValue>("plugin:hyprwalk:tiling_follow", "tiling mode's row goes with you (Y), else it stays where it is", true);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgTiling);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgTilingFollow);
    // plugin { hyprwalk { play_view = here } }
    g_cfgPlayView = makeShared<Config::Values::CStringValue>("plugin:hyprwalk:play_view",
                                                             "how P plays a window: here (where it is, the view as it was) or fill (facing it, filling the view); Shift+P the other", "here");
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgPlayView);
    // plugin { hyprwalk { play_size = 0.5 } }
    g_cfgPlaySize = makeShared<Config::Values::CFloatValue>("plugin:hyprwalk:play_size",
                                                            "played here in tiling mode's ring, how much of the view a window takes (0.25 to 0.94); Super+wheel changes it while you play",
                                                            PLAY_SIZE);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgPlaySize);

    g_p3D = std::make_unique<CDesktop3D>();
    g_p3D->init();

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprwalk", "toggle", luaToggle);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprwalk", "enter", luaEnter);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprwalk", "exit", luaExit);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprwalk", "type", luaType);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprwalk", "play", luaPlay);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprwalk", "away", luaAway);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprwalk", "tile", luaTile);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprwalk", "menu", luaMenu);

    log("loaded");
    return {"hyprwalk", "Walk around your desktop in first person", "hyprwalk", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_p3D)
        g_p3D->shutdown();
    g_p3D.reset();
}
