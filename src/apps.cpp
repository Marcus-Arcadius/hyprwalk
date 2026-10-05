#include "apps.hpp"

#include "globals.hpp"
#include "menu.hpp"

#include <hyprgraphics/image/Image.hpp>

#include <algorithm>
#include <cairo/cairo.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <unistd.h>
#include <unordered_set>

namespace fs = std::filesystem;

namespace hyprwalk {

    namespace {
        std::string env(const char* name, const std::string& fallback = "") {
            const char* v = std::getenv(name);
            return v && *v ? v : fallback;
        }

        std::vector<std::string> split(const std::string& s, char sep) {
            std::vector<std::string> out;
            std::string              cur;
            std::istringstream       in(s);
            while (std::getline(in, cur, sep))
                if (!cur.empty())
                    out.push_back(cur);
            return out;
        }

        std::string trim(const std::string& s) {
            const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? "" : s.substr(a, b - a + 1);
        }

        std::string lower(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        }

        // $XDG_DATA_HOME, then $XDG_DATA_DIRS in the spec's order; without $XDG_DATA_DIRS (a compositor with a bare
        // environment), NixOS's and Flatpak's dirs plus the default
        std::vector<fs::path> dataDirs() {
            const std::string     home = env("HOME", "/tmp"), user = env("USER");
            std::vector<fs::path> out{env("XDG_DATA_HOME", home + "/.local/share")};
            const std::string     dirs = env("XDG_DATA_DIRS",
                                             std::format("{}/.nix-profile/share:/etc/profiles/per-user/{}/share:/run/current-system/sw/share:{}/.local/share/flatpak/exports/share:"
                                                         "/var/lib/flatpak/exports/share:/usr/local/share:/usr/share",
                                                         home, user, home));
            for (const auto& d : split(dirs, ':'))
                out.emplace_back(d);
            return out;
        }

        // a desktop entry's string value: \s, \n, \t, \r and \\ (the spec's escapes)
        std::string unescape(const std::string& v) {
            std::string out;
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] != '\\' || i + 1 == v.size()) {
                    out += v[i];
                    continue;
                }
                switch (v[++i]) {
                    case 's': out += ' '; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    default: out += v[i];
                }
            }
            return out;
        }

        // Exec with field codes dropped (no files or URLs to pass; icon, name and file aren't needed)
        std::string stripFieldCodes(const std::string& exec) {
            std::string out;
            for (size_t i = 0; i < exec.size(); ++i) {
                if (exec[i] != '%' || i + 1 == exec.size()) {
                    out += exec[i];
                    continue;
                }
                if (exec[++i] == '%')
                    out += '%';
            }
            return trim(out);
        }

        bool inPath(const std::string& prog) {
            if (prog.find('/') != std::string::npos)
                return access(prog.c_str(), X_OK) == 0;
            for (const auto& d : split(env("PATH"), ':'))
                if (access((fs::path(d) / prog).c_str(), X_OK) == 0)
                    return true;
            return false;
        }

        bool listed(const std::string& list, const std::vector<std::string>& desktops) {
            for (const auto& d : split(list, ';'))
                if (std::ranges::find(desktops, d) != desktops.end())
                    return true;
            return false;
        }

        std::optional<SAppEntry> readEntry(const fs::path& file, const std::string& id, const std::vector<std::string>& desktops) {
            std::ifstream                                f(file);
            std::unordered_map<std::string, std::string> kv;
            bool                                         in = false;
            for (std::string line; std::getline(f, line);) {
                line = trim(line);
                if (line.empty() || line[0] == '#')
                    continue;
                if (line[0] == '[') {
                    in = line == "[Desktop Entry]";
                    continue;
                }
                const size_t eq = line.find('=');
                if (!in || eq == std::string::npos)
                    continue;
                const std::string key = trim(line.substr(0, eq));
                if (key.find('[') == std::string::npos && !kv.contains(key)) // (not the translations)
                    kv[key] = unescape(trim(line.substr(eq + 1)));
            }
            const auto get = [&](const char* k) { return kv.contains(k) ? kv[k] : std::string(); };
            if (get("Type") != "Application" || get("Hidden") == "true" || get("NoDisplay") == "true" || get("Exec").empty() || get("Name").empty())
                return std::nullopt;
            if (!get("OnlyShowIn").empty() && !listed(get("OnlyShowIn"), desktops))
                return std::nullopt;
            if (listed(get("NotShowIn"), desktops))
                return std::nullopt;
            if (!get("TryExec").empty() && !inPath(get("TryExec")))
                return std::nullopt;
            SAppEntry e;
            e.id       = id;
            e.name     = get("Name");
            e.comment  = get("GenericName").empty() ? get("Comment") : get("GenericName");
            e.icon     = get("Icon");
            e.exec     = stripFieldCodes(get("Exec"));
            e.wmClass  = get("StartupWMClass");
            e.terminal = get("Terminal") == "true";
            e.file     = file.string();
            return e;
        }

        // ------------------------------------------------------------------ icons

        std::vector<std::string> iconThemes() {
            std::vector<std::string> out;
            // the GTK icon theme first, as GTK apps use it
            std::ifstream ini(env("XDG_CONFIG_HOME", env("HOME", "/tmp") + "/.config") + "/gtk-3.0/settings.ini");
            for (std::string line; std::getline(ini, line);) {
                if (const size_t eq = line.find('='); eq != std::string::npos && trim(line.substr(0, eq)) == "gtk-icon-theme-name")
                    out.push_back(trim(line.substr(eq + 1)));
            }
            for (const char* t : {"hicolor", "Adwaita", "breeze", "Papirus"})
                if (std::ranges::find(out, t) == out.end())
                    out.emplace_back(t);
            return out;
        }

        std::optional<fs::path> findIcon(const std::string& name) {
            std::error_code ec;
            if (name.starts_with("/"))
                return fs::is_regular_file(name, ec) ? std::optional<fs::path>(name) : std::nullopt;
            std::vector<fs::path> bases{fs::path(env("HOME", "/tmp")) / ".icons"};
            for (const auto& d : dataDirs())
                bases.push_back(d / "icons");
            static constexpr const char* SIZES[] = {"scalable", "256x256", "128x128", "96x96", "64x64", "48x48", "32x32"};
            for (const auto& theme : iconThemes()) {
                for (const auto& base : bases) {
                    const fs::path t = base / theme;
                    if (!fs::is_directory(t, ec))
                        continue;
                    for (const char* size : SIZES) {
                        for (const char* ext : {".svg", ".png"}) {
                            // THEME/SIZE/apps (hicolor, Adwaita, Papirus) and THEME/apps/SIZE (breeze)
                            for (const auto& p : {t / size / "apps" / (name + ext), t / "apps" / std::string(size).substr(0, std::string(size).find('x')) / (name + ext)})
                                if (fs::is_regular_file(p, ec))
                                    return p;
                        }
                    }
                }
            }
            for (const auto& d : dataDirs())
                for (const char* ext : {".png", ".svg"})
                    if (const fs::path p = d / "pixmaps" / (name + ext); fs::is_regular_file(p, ec))
                        return p;
            return std::nullopt;
        }

        std::shared_ptr<const SPicture> render(const fs::path& file, int size) {
            Hyprgraphics::CImage img(file.string(), {(double)size, (double)size});
            if (!img.success() || !img.cairoSurface() || !img.cairoSurface()->cairo())
                return nullptr;
            cairo_surface_t* src = img.cairoSurface()->cairo();
            const int        w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
            if (w < 1 || h < 1)
                return nullptr;
            auto pic = std::make_shared<SPicture>();
            pic->w = pic->h = size;
            pic->pixels.assign((size_t)size * size, 0);
            cairo_surface_t* dst = cairo_image_surface_create_for_data((unsigned char*)pic->pixels.data(), CAIRO_FORMAT_ARGB32, size, size, size * 4);
            cairo_t*         cr  = cairo_create(dst);
            const double     s   = std::min((double)size / w, (double)size / h);
            cairo_translate(cr, (size - w * s) / 2, (size - h * s) / 2);
            cairo_scale(cr, s, s);
            cairo_set_source_surface(cr, src, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            cairo_destroy(cr);
            cairo_surface_flush(dst);
            cairo_surface_destroy(dst);
            return pic;
        }
    }

    std::vector<SAppEntry> readDesktopEntries() {
        const auto                      desktops = split(env("XDG_CURRENT_DESKTOP", "Hyprland"), ':');
        std::vector<SAppEntry>          out;
        std::unordered_set<std::string> seen;
        std::error_code                 ec;
        for (const auto& base : dataDirs()) {
            const fs::path dir = base / "applications";
            if (!fs::is_directory(dir, ec))
                continue;
            std::vector<fs::path> files;
            for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec)
                    break;
                if (it->path().extension() == ".desktop")
                    files.push_back(it->path());
            }
            std::ranges::sort(files);
            for (const auto& f : files) {
                // its desktop id: the path under applications/ with / as -, less .desktop
                std::string id = fs::relative(f, dir, ec).string();
                std::ranges::replace(id, '/', '-');
                id.resize(id.size() - 8);
                if (!seen.insert(id).second)
                    continue; // an earlier dir's wins, even a hidden one
                if (auto e = readEntry(f, id, desktops))
                    out.push_back(std::move(*e));
            }
        }
        std::ranges::sort(out, [](const SAppEntry& a, const SAppEntry& b) { return lower(a.name) < lower(b.name); });
        return out;
    }

    const SAppEntry* findApp(const std::vector<SAppEntry>& apps, const std::string& what) {
        std::string w = what;
        if (w.ends_with(".desktop"))
            w.resize(w.size() - 8);
        const std::string lw = lower(w);
        for (const auto& a : apps)
            if (a.id == w || fs::path(a.file).filename() == what)
                return &a;
        for (const auto& a : apps)
            if (lower(a.id) == lw || lower(a.name) == lw)
                return &a;
        // the last part of a reverse-DNS id: "firefox" for org.mozilla.firefox
        for (const auto& a : apps)
            if (const size_t dot = a.id.rfind('.'); dot != std::string::npos && lower(a.id.substr(dot + 1)) == lw)
                return &a;
        return nullptr;
    }

    const SAppEntry* appForClass(const std::vector<SAppEntry>& apps, const std::string& cls) {
        const std::string lc = lower(cls);
        for (const auto& a : apps)
            if (!a.wmClass.empty() && lower(a.wmClass) == lc)
                return &a;
        return findApp(apps, cls);
    }

    std::string inTerminal(const std::string& cmd) {
        std::string term = env("TERMINAL");
        if (term.empty())
            for (const char* t : {"foot", "kitty", "alacritty", "wezterm", "xterm"})
                if (inPath(t)) {
                    term = t;
                    break;
                }
        if (term.empty())
            return cmd;
        const std::string name = fs::path(split(term, ' ').front()).filename().string();
        if (name == "wezterm")
            return term + " start -- " + cmd;
        return name == "foot" || name == "kitty" ? term + " " + cmd : term + " -e " + cmd;
    }

    std::shared_ptr<const SPicture> appIcon(const std::string& icon, int size, bool load) {
        static std::mutex                                                       lock;
        static std::unordered_map<std::string, std::shared_ptr<const SPicture>> cache;
        if (icon.empty() || size < 1)
            return nullptr;
        const std::string           key = std::format("{}@{}", icon, size);
        std::lock_guard<std::mutex> g(lock);
        if (const auto it = cache.find(key); it != cache.end())
            return it->second;
        if (!load)
            return nullptr;
        const auto file = findIcon(icon);
        auto       pic  = file ? render(*file, size) : nullptr;
        cache[key]      = pic;
        return pic;
    }

    // ------------------------------------------------------------------ rules

    std::vector<SAppRule> parseAppRules(const std::string& spec, std::string& error) {
        std::vector<SAppRule> out;
        for (const auto& part : split(spec, ',')) {
            const std::string p = trim(part);
            if (p.empty() || p == "[[EMPTY]]")
                continue;
            const size_t colon = p.rfind(':');
            if (colon == std::string::npos) {
                error = std::format("app rule \"{}\": CLASS: DISTANCE [HEIGHT|auto] [left|right|SIDE]", p);
                continue;
            }
            SAppRule           r;
            r.pattern = trim(p.substr(0, colon));
            std::istringstream in(p.substr(colon + 1));
            // height: metres or auto (the default: screen size); a side in metres needs a height or auto before it
            std::string height, side;
            if (!(in >> r.distance) || r.distance < 0.3f) {
                error = std::format("app rule \"{}\": the distance is metres (0.3 or more)", p);
                continue;
            }
            if (in >> height) {
                if (height == "left" || height == "right")
                    side = height;
                else if (height != "auto") {
                    char* end = nullptr;
                    r.height  = std::strtof(height.c_str(), &end);
                    if (end == height.c_str() || *end || !std::isfinite(r.height) || r.height < 0.05f) {
                        error = std::format("app rule \"{}\": the height is metres (0.05 or more) or auto", p);
                        continue;
                    }
                }
            }
            if (side.empty())
                in >> side;
            if (!side.empty())
                r.side = side == "left" ? -1.f : side == "right" ? 1.f : std::strtof(side.c_str(), nullptr);
            try {
                (void)std::regex(r.pattern, std::regex::icase);
            } catch (const std::regex_error&) {
                error = std::format("app rule \"{}\": not a regular expression", p);
                continue;
            }
            out.push_back(std::move(r));
        }
        return out;
    }

    SAppRule appRule(const std::vector<SAppRule>& user, const std::string& cls) {
        // no fixed heights, so windows open as big as on your screen (a fixed one shrank tall windows)
        static const std::vector<SAppRule> BUILT_IN = {
            // games (Steam's, Proton's, gamescope's): further off
            {"steam_app_.*|gamescope|.*\\.exe|steam_proton|chocolate-doom|supertux2|retroarch|.*minecraft.*|hyprwalkgame.*", 2.0f, 0.f, 0.f},
            // chat and calls: to the left, closer
            {"discord|vesktop|webcord|equibop|signal|telegram.*|org\\.telegram\\..*|element|slack|zoom|teams.*", 1.3f, 0.f, -1.f},
            // videos
            {"mpv|vlc|org\\.videolan\\.vlc|io\\.github\\.celluloid_player\\.celluloid|.*\\.showtime", 2.0f, 0.f, 0.f},
        };
        for (const auto* rules : {&user, &BUILT_IN})
            for (const auto& r : *rules) {
                try {
                    if (std::regex_match(cls, std::regex(r.pattern, std::regex::icase)))
                        return r;
                } catch (const std::regex_error&) {}
            }
        return {};
    }

    // ------------------------------------------------------------------ spots

    std::string windowSpotsPath(const std::string& mapPath) {
        const std::string dir = env("XDG_STATE_HOME", env("HOME", "/tmp") + "/.local/state") + "/hyprwalk/windows";
        if (mapPath.empty())
            return dir + "/courtyard.conf";
        uint32_t h = 2166136261u;
        for (unsigned char c : mapPath) {
            h ^= c;
            h *= 16777619u;
        }
        return std::format("{}/{}-{:08x}.conf", dir, fs::path(mapPath).stem().string(), h);
    }

    std::unordered_map<std::string, SWindowSpot> readWindowSpots(const std::string& mapPath) {
        std::unordered_map<std::string, SWindowSpot> out;
        std::ifstream                                f(windowSpotsPath(mapPath));
        for (std::string line; std::getline(f, line);) {
            std::istringstream in(line);
            std::string        key, cls;
            SWindowSpot        s;
            if (!(in >> key) || key != "window")
                continue;
            if (!(in >> s.center.x >> s.center.y >> s.center.z >> s.rot.x >> s.rot.y >> s.rot.z >> s.rot.w >> s.scale) || s.scale <= 0)
                continue;
            std::getline(in, cls);
            cls = trim(cls);
            if (!cls.empty()) {
                s.rot    = s.rot.normalized();
                out[cls] = s;
            }
        }
        return out;
    }

    bool saveWindowSpots(const std::string& mapPath, const std::unordered_map<std::string, SWindowSpot>& spots) {
        const auto      file = windowSpotsPath(mapPath);
        std::error_code ec;
        fs::create_directories(fs::path(file).parent_path(), ec);
        std::ofstream f(file, std::ios::trunc);
        if (!f)
            return false;
        f << std::format("# hyprwalk: where windows were put, by class, for {}\n", mapPath.empty() ? "the courtyard" : mapPath);
        std::vector<std::string> classes;
        for (const auto& [cls, s] : spots)
            classes.push_back(cls);
        std::ranges::sort(classes);
        for (const auto& cls : classes) {
            const auto& s = spots.at(cls);
            f << std::format("window {} {} {} {} {} {} {} {} {}\n", s.center.x, s.center.y, s.center.z, s.rot.x, s.rot.y, s.rot.z, s.rot.w, s.scale, cls);
        }
        return (bool)f;
    }
}
