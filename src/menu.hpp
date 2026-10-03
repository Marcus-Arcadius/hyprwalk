#pragma once

#include "avatar.hpp"
#include "renderer.hpp"

#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace h3d {

    // a picture for an item instead of its emoji (an app's icon)
    struct SPicture {
        int                   w = 0, h = 0;
        std::vector<uint32_t> pixels; // cairo's premultiplied ARGB
    };

    // an Action Menu ring entry: a page to open, or an action for the owner
    struct SMenuItem {
        std::string label;
        std::string hint;                          // smaller second line (its setting), "" = none
        std::string icon;                          // an emoji, "" = none
        std::shared_ptr<const SPicture> picture;   // drawn instead of the emoji, null = none
        std::string page;                          // the page it opens, "" = an action
        int         action = 0, arg = 0, arg2 = 0; // owner-defined
        std::string target;                        // owner-defined target (a window, an app)
        bool        on = false, disabled = false;
        bool        dial  = false; // slider: a dial (VRChat radial puppet) sets value
        int         axes  = 1;     // 2: two-axis stick; value, value2 in -1..1
        float       value = 0;     // 0..1
        float       value2 = 0;    // a stick's y, up > 0
        bool        operator==(const SMenuItem&) const = default;
    };

    struct SMenuPage {
        std::string            title; // "" = no such page
        std::vector<SMenuItem> items;
        bool                   operator==(const SMenuPage&) const = default;
    };

    // VRChat-style Action Menu: up to eight items (nine on the root) round a back button in the middle; longer pages
    // get a "More" slot. The owner supplies the pages every frame. While open, the mouse moves its cursor instead of
    // the camera: click picks, the wheel goes round, 1-9 pick directly.
    class CActionMenu {
      public:
        using FPages = std::function<SMenuPage(const std::string& id)>;
        using FDial  = std::function<void(const SMenuItem& item, float value, float value2)>; // a slider's dial moved
        static constexpr int  SLOTS      = 8;
        static constexpr int  ROOT_SLOTS = 9; // root page: a 9th slot (Avatars) keeps the others' numbers
        static constexpr auto ROOT       = "main";

        explicit CActionMenu(FPages pages, FDial dial = {}) : m_pages(std::move(pages)), m_onDial(std::move(dial)) {}

        bool open() const { // taking input
            return !m_stack.empty();
        }
        bool visible() const { // drawn: open, or fading out
            return m_fade > 0.f;
        }
        bool show(const std::string& page = ROOT); // a path as path() gives; false = no such page
        void hide();
        void back();                   // on the root it closes; on a dial, back to its page
        void move(float dx, float dy); // cursor in logical px; sets a dial's value
        void scroll(int steps);        // next/previous pickable item; on a dial, 5% a step
        // handles pages, "More" and the middle, returning owner actions. Slots go clockwise from the top, -1 = middle;
        // on a dial a slot sets the value (first 0%, last 100%) and a pick goes back
        std::optional<SMenuItem> pick(); // what the cursor points at
        std::optional<SMenuItem> pick(int slot);
        int                      highlighted() const; // -1 = the middle, -2 = nothing
        const SMenuPage&         page() const {       // as shown: up to eight items (the root nine)
            return m_page;
        }
        std::string path() const; // e.g. "main/emotes", "emotes:2", "outfit/~Hue"
        const SMenuItem* dial() const { // the slider being set, null = none
            return m_dial ? &*m_dial : nullptr;
        }

        // every frame: fades, refetches the page and redraws it when it changed
        void      update(float dt, int outW, int outH, float scale);
        // position and size on an output of that size (update() calls it); cursor moves are in its logical pixels
        void      layout(int outW, int outH, float scale);
        SHudImage hud() const;

      private:
        struct SLevel {
            std::string id;
            int         chunk = 0; // of a page with more than fits
        };

        FPages                   m_pages;
        FDial                    m_onDial;
        std::optional<SMenuItem> m_dial; // a slider's dial over the page
        float                    m_drawnValue = -1, m_drawnValue2 = 0;
        std::vector<SLevel>   m_stack; // empty = closed
        SMenuPage             m_page;
        float                 m_cx = 0, m_cy = 0; // the cursor, in radii from the middle, y down
        float                 m_fade = 0;
        float                 m_flash = 0; // a slot just picked lights up
        int                   m_flashSlot = -1;
        int                   m_R             = 0; // its radius, output pixels
        float                 m_radiusLogical = 300;
        float                 m_x = 0, m_y = 0; // center, output pixels

        std::vector<uint32_t> m_pixels; // cairo premultiplied ARGB, m_size square
        int                   m_size = 0;
        uint64_t              m_serial = 0;
        // last drawn
        SMenuPage             m_drawnPage;
        int                   m_drawnR = 0, m_drawnHighlight = -3, m_drawnFlash = 0;

        void refresh(); // m_page from the top of the stack
        void aimAt(int slot);
        void draw(int R, int highlight, int flash);
        void setDial(float value, float value2 = 0); // sets the dial's value(s) and tells the owner
    };

    enum eMenuAction : uint8_t {
        MA_NONE,
        MA_EMOTE,      // arg: the animator's emote
        MA_EXPRESSION, // arg: the model's expression
        MA_GESTURE,    // arg: the hand (0 left, 1 right, 2 both), arg2: eGesture
        MA_TOGGLE,     // arg: the model's toggle
        MA_PART,       // arg: the model's part (and same-named ones)
        MA_OUTFIT_RESET,
        MA_VIEW,
        MA_PHYSICS,
        MA_FLY,
        MA_RESPAWN,
        MA_FACE_RESET,
        MA_EMOTE_STOP,
        MA_SLIDER, // arg: the model's slider; its dial sets it
        MA_LIPSYNC, // toggles lip sync
        MA_LIPSYNC_GAIN, // dial: lip sync mic gain (0 = automatic, up to 60 dB)
        // the plugin's own pages (main.cpp): apps, windows, maps and avatars
        MA_LAUNCH, // target: a desktop id, or a command
        MA_WINDOW, // target: the window's address; arg: eWindowAction
        MA_TILING, // tiling mode (T) on or off
        MA_TILING_FOLLOW, // tiling row follows you or stays (Y)
        MA_MENU_BACK,     // back a page (the Close page's "Keep it")
        MA_MAP,           // target: the map's file, "" = the courtyard
        MA_AVATAR,        // target: the avatar's file
    };

    // what the Windows page does to a window
    enum eWindowAction : uint8_t {
        WA_FOCUS,   // show its workspace, give it the keyboard
        WA_BRING,   // out in the world, in front of you
        WA_WALL,    // back on the desktop wall
        WA_PIN,     // follows the view in a corner; again unpins it
        WA_BIGGER,  // real size +25% (the app redraws at it)
        WA_SMALLER, // a fifth less
        WA_PLAY,    // play mode on it
        WA_CLOSE,   // asked to close
    };

    // what the pages show
    struct SActionState {
        const SAvatarModel*    avatar  = nullptr; // its name is the Avatars item's hint
        const CAvatarAnimator* anim    = nullptr;
        bool                   loading = false; // an avatar is loading (Avatars hint: loading…)
        bool                   third = false, fly = false;
        bool                   lipsync = false, microphone = true; // lip sync on; a microphone exists
        float                  micGain = NAN, micGainNow = 0;      // set gain (NAN: automatic) and current gain, dB
        std::string            map;                                // Maps hint: the world shown, or loading…
    };
    constexpr float MIC_GAIN_MAX = 60.f; // dB, the gain dial's end

    // main, emotes, expressions, gestures, left, right, both, outfit, parts, options
    SMenuPage actionPage(const std::string& id, const SActionState& s);

    // corner badge (red dot, a few words, dark rounded box) in cairo premultiplied ARGB at `scale`; sets w, h
    void drawBadge(std::vector<uint32_t>& pixels, int& w, int& h, const std::string& text, float scale);
}
