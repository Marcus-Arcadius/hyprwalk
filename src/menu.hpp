#pragma once

#include "avatar.hpp"
#include "renderer.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace h3d {

    // one thing in the Action Menu's ring: a page to open, or something for the owner to do
    struct SMenuItem {
        std::string label;
        std::string hint;                          // a smaller line under it (what it's set to), "" = none
        std::string icon;                          // an emoji, "" = none
        std::string page;                          // the page it opens, "" = an action
        int         action = 0, arg = 0, arg2 = 0; // the owner's
        bool        on = false, disabled = false;
        bool        dial  = false; // a slider: picking it opens a dial (VRChat's radial puppet) that sets value
        int         axes  = 1;     // 2: the dial is a stick (a two-axis puppet) that sets value and value2, -1..1 each
        float       value = 0;     // 0..1
        float       value2 = 0;    // a stick's y, up > 0
        bool        operator==(const SMenuItem&) const = default;
    };

    struct SMenuPage {
        std::string            title; // "" = no such page
        std::vector<SMenuItem> items;
        bool                   operator==(const SMenuPage&) const = default;
    };

    // The Action Menu, like VRChat's: a ring of up to eight things to pick around a button in the middle that goes
    // back; a page with more gets a "More" slot. The owner makes the pages, and they're asked for again every frame so
    // they show what's on. While it's open the mouse moves a cursor in it instead of turning the camera: a click picks
    // what the cursor points at, the wheel goes round, and 1-8 pick directly.
    class CActionMenu {
      public:
        using FPages = std::function<SMenuPage(const std::string& id)>;
        using FDial  = std::function<void(const SMenuItem& item, float value, float value2)>; // a slider's dial moved
        static constexpr int  SLOTS = 8;
        static constexpr auto ROOT  = "main";

        explicit CActionMenu(FPages pages, FDial dial = {}) : m_pages(std::move(pages)), m_onDial(std::move(dial)) {}

        bool open() const { // taking input
            return !m_stack.empty();
        }
        bool visible() const { // drawn: open, or fading out
            return m_fade > 0.f;
        }
        bool show(const std::string& page = ROOT); // a page other than the root goes back to it; false = no such page
        void hide();
        void back();                   // on the root it closes; on a dial, back to its page
        void move(float dx, float dy); // the cursor, logical pixels; on a dial, where it points round sets the value, on
                                       // a stick where it is
        void scroll(int steps);        // to the next item round that can be picked, or back; on a dial, 5% a step
        // Pages, "More" and the middle are dealt with here; something for the owner to do comes back. The slot
        // counts from 0 clockwise from the top, -1 = the middle. A slider opens its dial; on the dial, a pick goes
        // back to the page, and a slot sets the value (the first 0%, the last 100%).
        std::optional<SMenuItem> pick(); // what the cursor points at
        std::optional<SMenuItem> pick(int slot);
        int                      highlighted() const; // the slot the cursor points at, -1 = the middle, -2 = nothing
        const SMenuPage&         page() const {       // as shown: up to eight items
            return m_page;
        }
        std::string path() const; // "main/emotes", a later part of a long page as "emotes:2", a dial "outfit/~Hue"
        const SMenuItem* dial() const { // the slider being set, null = none
            return m_dial ? &*m_dial : nullptr;
        }

        // every frame: fades, asks for the page again and draws it anew when it looks different
        void      update(float dt, int outW, int outH, float scale);
        // where it goes and how big it is on an output of that size (update() does it): the mouse moves the cursor by
        // logical pixels of it
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
        float                 m_x = 0, m_y = 0; // where its middle is, output pixels

        std::vector<uint32_t> m_pixels; // the picture: cairo's premultiplied ARGB, m_size square
        int                   m_size = 0;
        uint64_t              m_serial = 0;
        // what it shows
        SMenuPage             m_drawnPage;
        int                   m_drawnR = 0, m_drawnHighlight = -3, m_drawnFlash = 0;

        void refresh(); // m_page from the top of the stack
        void aimAt(int slot);
        void draw(int R, int highlight, int flash);
        void setDial(float value, float value2 = 0); // the dial's value (a stick's two), and the owner told
    };

    // the plugin's actions
    enum eMenuAction : uint8_t {
        MA_NONE,
        MA_EMOTE,      // arg: the animator's emote
        MA_EXPRESSION, // arg: the model's expression
        MA_GESTURE,    // arg: the hand (0 left, 1 right, 2 both), arg2: eGesture
        MA_TOGGLE,     // arg: the model's toggle
        MA_PART,       // arg: the model's part (and the others of its name)
        MA_OUTFIT_RESET,
        MA_VIEW,
        MA_PHYSICS,
        MA_FLY,
        MA_RESPAWN,
        MA_FACE_RESET,
        MA_EMOTE_STOP,
        MA_SLIDER, // arg: the model's slider; its dial sets it
        MA_LIPSYNC, // the microphone moves the mouth, or not
    };

    // what the pages show
    struct SActionState {
        const SAvatarModel*    avatar  = nullptr;
        const CAvatarAnimator* anim    = nullptr;
        bool                   loading = false; // an avatar is on its way
        bool                   third = false, fly = false;
        bool                   lipsync = false, microphone = true; // on; there is a microphone to have it on with
    };

    // main, emotes, expressions, gestures, left, right, both, outfit, parts, options
    SMenuPage actionPage(const std::string& id, const SActionState& s);

    // a badge for a corner of the screen: a red dot and a few words on a dark rounded box, in cairo's premultiplied
    // ARGB (the menu's), `scale` pixels a logical one; w and h what it came to
    void drawBadge(std::vector<uint32_t>& pixels, int& w, int& h, const std::string& text, float scale);
}
