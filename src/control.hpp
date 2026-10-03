#pragma once

#include "avatar.hpp"
#include "menu.hpp"

#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace h3d {

    std::string jsonEscape(const std::string& s);

    // What the plugin's avatar commands (hyprctl hypr3d avatar ..., the Action Menu's items and dials) do to the avatar,
    // apart from Hyprland: main.cpp hands them over, and the offscreen harness (tools/test/harness/shot.cpp --ctl, --key)
    // runs the same code. It keeps what was set by hand for when the avatar is loaded again.
    class CAvatarControl {
      public:
        explicit CAvatarControl(CAvatarAnimator& anim) : m_anim(anim) {}

        std::shared_ptr<SAvatarModel> avatar;                 // null = none
        bool                          loading       = false; // an avatar is on its way
        bool                          emotesLoading = false; // emote files are
        std::string                   emoteSound    = "null"; // (JSON) what plays an emote's sound, as the plugin has it
        std::string                   expression;            // held, by the name it was asked for (kept for the next avatar)
        float                         expressionWeight = 1;
        // set by hand, for when the avatar is loaded again
        std::string                                        outfitFor; // the avatar they're for
        std::vector<std::pair<std::string, int>>           outfitSet;
        std::vector<std::pair<std::string, float>>         shapesSet;
        std::vector<std::tuple<std::string, float, float>> slidersSet; // (x, a 2D one's y)

        // a new avatar is in the animator: what was set by hand, again
        void loaded();
        // hyprctl hypr3d avatar expression|gesture|emote|emotes|parts|toggle|shape|slider|physics|attack ...; "" = not one
        // of these. An emote file or folder goes to loadEmote(file, loop), whose answer it gives
        std::string command(const std::vector<std::string>& args, const std::string& rest,
                            const std::function<std::string(const std::string& file, int loop)>& loadEmote);
        // a toggle, else the parts of that name; state 1 on (shown), 0 off (hidden), -1 as the settings file has it,
        // 2 the other way (then it's what it came to)
        std::string setOutfit(const std::string& name, int& state);
        std::string changeOutfit(const std::string& name, int state); // setOutfit, kept for the next load
        void        changeSlider(const std::string& name, float value, float valueY = NAN); // NaN = as it starts
        void        resetOutfit();
        // one of the Action Menu's items for the avatar (MA_EMOTE ... MA_FACE_RESET); "" = not one of those
        std::string action(const SMenuItem& item);
        void        dial(const SMenuItem& item, float value, float value2); // a slider's dial moved (a stick: both)

      private:
        CAvatarAnimator& m_anim;

        std::string noAvatar() const;
        std::string face(const std::vector<std::string>& args);
        std::string outfit(const std::vector<std::string>& args);
        std::string emote(const std::vector<std::string>& args, std::string rest,
                          const std::function<std::string(const std::string& file, int loop)>& loadEmote);
    };

    // the Action Menu's input as the plugin takes it (evdev codes); true = the menu took it. A pick goes to pick
    using FMenuPick = std::function<void(const std::optional<SMenuItem>&)>;
    bool menuKey(CActionMenu& menu, uint32_t key, const FMenuPick& pick); // Esc, Backspace, Enter, 1-9
    bool menuButton(CActionMenu& menu, uint32_t button, const FMenuPick& pick); // left picks, right back, middle closes
    void menuWheel(CActionMenu& menu, float& fraction, float notches);          // round it, a notch an item
    // hyprctl hypr3d menu [open [page]|close|toggle|back|pick [n]|move dx dy|scroll n] (the menu's state without
    // any); action does what a pick picked
    std::string menuStatus(const CActionMenu& menu);
    std::string menuCommand(CActionMenu& menu, const std::vector<std::string>& args, const std::function<std::string(const SMenuItem&)>& action);
}
