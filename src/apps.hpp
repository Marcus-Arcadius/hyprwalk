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

    // an app from the XDG desktop entries (applications/*.desktop in the XDG data dirs)
    struct SAppEntry {
        std::string id;      // its desktop id: "org.mozilla.firefox" for org.mozilla.firefox.desktop
        std::string name, comment, icon;
        std::string exec;    // its Exec line with the field codes (%f, %U, ...) gone
        std::string wmClass; // StartupWMClass: the class its window will have, "" = not said
        std::string file;
        bool        terminal = false;
    };

    // The entries that show in menus (not NoDisplay or Hidden, for this desktop, their TryExec there), by name.
    // Earlier data dirs win over later ones, $XDG_DATA_HOME first, as the spec has it
    std::vector<SAppEntry> readDesktopEntries();
    // a desktop id (with or without .desktop), a file name, or a name (any case); null = none
    const SAppEntry* findApp(const std::vector<SAppEntry>& apps, const std::string& what);
    // the app a window of this class belongs to: its StartupWMClass, else as findApp; null = none
    const SAppEntry* appForClass(const std::vector<SAppEntry>& apps, const std::string& cls);
    // a command to run in a terminal ($TERMINAL, else foot, kitty, alacritty, wezterm or xterm, whichever is there)
    std::string inTerminal(const std::string& cmd);
    // an app's icon (a theme icon's name, or a file), from the icon themes and pixmaps: PNG, SVG and whatever
    // hyprgraphics reads, `size` px square, cairo's premultiplied ARGB; null = none (or, with load false, not loaded
    // yet: finding and drawing it takes a moment). Kept once loaded
    std::shared_ptr<const SPicture> appIcon(const std::string& icon, int size, bool load = true);

    // Where a window launched from 3D opens: metres in front of the eye, how tall it's made there (0 = auto: as big as
    // it looks on your screen, made smaller to fit your view), and to the side (> 0 right), turned to face you. The
    // config's plugin:hypr3d:app_rules, "CLASS: DISTANCE [HEIGHT|auto] [left|right|SIDE]" separated by commas (CLASS a
    // regular expression, whole, any case), then the built-in ones: games and videos further, chat apps at the side
    struct SAppRule {
        std::string pattern;
        float       distance = 1.5f, height = 0.f, side = 0.f;
    };
    std::vector<SAppRule> parseAppRules(const std::string& spec, std::string& error);
    SAppRule              appRule(const std::vector<SAppRule>& user, const std::string& cls);

    // where windows were put in the world, by class, for a map ("" = the courtyard): kept across sessions next to the
    // map's start and desktop place (map.cpp's state files)
    struct SWindowSpot {
        V3    center;
        Quat  rot;
        float scale = 0; // metres per logical px
    };
    std::string                                  windowSpotsPath(const std::string& mapPath);
    std::unordered_map<std::string, SWindowSpot> readWindowSpots(const std::string& mapPath);
    bool                                         saveWindowSpots(const std::string& mapPath, const std::unordered_map<std::string, SWindowSpot>& spots);
}
