#pragma once

#include "math3d.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace h3d {

    struct SPicture; // menu.hpp

    // an XDG desktop entry (applications/*.desktop in the XDG data dirs)
    struct SAppEntry {
        std::string id;      // desktop id, e.g. "org.mozilla.firefox"
        std::string name, comment, icon;
        std::string exec;    // Exec without field codes (%f, %U, ...)
        std::string wmClass; // StartupWMClass (its window's class), "" = unset
        std::string file;
        bool        terminal = false;
    };

    // entries shown in menus (not NoDisplay/Hidden, for this desktop, TryExec found), by name; earlier data dirs win
    std::vector<SAppEntry> readDesktopEntries();
    // a desktop id (with or without .desktop), a file name, or a name (any case); null = none
    const SAppEntry* findApp(const std::vector<SAppEntry>& apps, const std::string& what);
    // the app a window of this class belongs to: its StartupWMClass, else as findApp; null = none
    const SAppEntry* appForClass(const std::vector<SAppEntry>& apps, const std::string& cls);
    // a command to run in a terminal ($TERMINAL, else foot, kitty, alacritty, wezterm or xterm, whichever is there)
    std::string inTerminal(const std::string& cmd);
    // an app's icon (theme icon name or file) from icon themes and pixmaps, `size` px square, cairo premultiplied ARGB,
    // kept once loaded; null = none, or with load false not loaded yet (finding and drawing it takes a moment)
    std::shared_ptr<const SPicture> appIcon(const std::string& icon, int size, bool load = true);

    // where a window launched in 3D opens, facing you: metres ahead, height (0 = auto: its screen size, shrunk to fit
    // the view) and side offset (> 0 right). plugin:hypr3d:app_rules is "CLASS: DISTANCE [HEIGHT|auto]
    // [left|right|SIDE]", comma separated (CLASS a whole-match regex, any case), then built-in rules: games and videos
    // further, chat apps to the side
    struct SAppRule {
        std::string pattern;
        float       distance = 1.5f, height = 0.f, side = 0.f;
    };
    std::vector<SAppRule> parseAppRules(const std::string& spec, std::string& error);
    SAppRule              appRule(const std::vector<SAppRule>& user, const std::string& cls);

    // where windows were put in a map ("" = the courtyard), by class; saved next to map.cpp's state files
    struct SWindowSpot {
        V3    center;
        Quat  rot;
        float scale = 0; // metres per logical px
    };
    std::string                                  windowSpotsPath(const std::string& mapPath);
    std::unordered_map<std::string, SWindowSpot> readWindowSpots(const std::string& mapPath);
    bool                                         saveWindowSpots(const std::string& mapPath, const std::unordered_map<std::string, SWindowSpot>& spots);
}
