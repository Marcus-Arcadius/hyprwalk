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

    // Avatar commands (hyprctl hypr3d avatar ..., Action Menu items and dials) apart from Hyprland, so main.cpp and the
    // offscreen harness (tools/test/harness/shot.cpp --ctl, --key) share them. Remembers manual settings for reloads.
    class CAvatarControl {
      public:
        explicit CAvatarControl(CAvatarAnimator& anim) : m_anim(anim) {}

        std::shared_ptr<SAvatarModel> avatar;                 // null = none
        bool                          loading       = false; // an avatar is loading
        bool                          emotesLoading = false; // emote files are
        std::string                   emoteSound    = "null"; // JSON: the emote sound's state
        std::string                   expression;            // held expression's requested name (kept across avatars)
        float                         expressionWeight = 1;
        // manual settings, reapplied when the avatar reloads
        std::string                                        outfitFor; // the avatar they're for
        std::vector<std::pair<std::string, int>>           outfitSet;
        std::vector<std::pair<std::string, float>>         shapesSet;
        std::vector<std::tuple<std::string, float, float>> slidersSet; // (x, a 2D one's y)

        // a new avatar is in the animator: reapply manual settings
        void loaded();
        // hyprctl hypr3d avatar expression|gesture|emote|emotes|parts|toggle|shape|slider|physics|attack ...; "" = not
        // one of these. Emote files and folders go to loadEmote(file, loop), whose answer it returns
        std::string command(const std::vector<std::string>& args, const std::string& rest,
                            const std::function<std::string(const std::string& file, int loop)>& loadEmote);
        // a toggle, else the parts of that name; state 1 shown, 0 hidden, -1 settings default, 2 flip (gets the result)
        std::string setOutfit(const std::string& name, int& state);
        std::string changeOutfit(const std::string& name, int state); // setOutfit, kept for the next load
        void        changeSlider(const std::string& name, float value, float valueY = NAN); // NaN = default
        void        resetOutfit();
        // an Action Menu avatar item (MA_EMOTE .. MA_FACE_RESET); "" = not one
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

    // Action Menu input (evdev codes); true = the menu took it; picks go to `pick`
    using FMenuPick = std::function<void(const std::optional<SMenuItem>&)>;
    bool menuKey(CActionMenu& menu, uint32_t key, const FMenuPick& pick); // Esc, Backspace, Enter, 1-9
    bool menuButton(CActionMenu& menu, uint32_t button, const FMenuPick& pick); // left picks, right back, middle closes
    void menuWheel(CActionMenu& menu, float& fraction, float notches);          // a notch per item
    // hyprctl hypr3d menu [open [page]|close|toggle|back|pick [n]|move dx dy|scroll n]; `action` runs a pick
    std::string menuStatus(const CActionMenu& menu);
    std::string menuCommand(CActionMenu& menu, const std::vector<std::string>& args, const std::function<std::string(const SMenuItem&)>& action);
}
