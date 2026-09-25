#include "control.hpp"

#include "gltf.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>

namespace h3d {

    std::string jsonEscape(const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                default:
                    if ((unsigned char)c < 0x20)
                        out += std::format("\\u{:04x}", (int)c);
                    else
                        out += c;
            }
        }
        return out;
    }

    namespace {
        // evdev codes
        enum : uint32_t {
            K_ESC       = 1,
            K_1         = 2, // .. K_8 = 9
            K_8         = 9,
            K_BACKSPACE = 14,
            K_E         = 18,
            K_ENTER     = 28,
            K_G         = 34,
            K_X         = 45,
            K_KPENTER   = 96,
            BTN_LEFT_   = 0x110,
            BTN_RIGHT_  = 0x111,
            BTN_MIDDLE_ = 0x112,
        };

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
    }

    std::string CAvatarControl::noAvatar() const {
        return loading ? "error: the avatar is still loading" : "error: no avatar loaded";
    }

    void CAvatarControl::loaded() {
        if (!avatar)
            return;
        if (!expression.empty())
            m_anim.setExpression(avatar->findExpression(expression), expressionWeight);
        if (outfitFor != avatar->path) {
            outfitFor = avatar->path;
            outfitSet.clear();
            shapesSet.clear();
            slidersSet.clear();
        }
        for (auto [name, state] : outfitSet)
            setOutfit(name, state);
        for (const auto& [name, w] : shapesSet)
            for (const int m : avatar->findMorphs(name))
                m_anim.setShape(m, w);
        for (const auto& [name, v, vy] : slidersSet)
            m_anim.setSlider(avatar->findSlider(name), v, vy);
    }

    std::string CAvatarControl::command(const std::vector<std::string>& args, const std::string& rest,
                                        const std::function<std::string(const std::string&, int)>& loadEmote) {
        const std::string sub = args.size() > 1 ? args[1] : "";
        if (sub == "expression" || sub == "gesture")
            return face(args);
        if (sub == "emote" || sub == "emotes")
            return emote(args, rest, loadEmote);
        if (sub == "parts" || sub == "toggle" || sub == "shape" || sub == "slider")
            return outfit(args);
        if (sub == "physics") { // hair, skirts and the like swing (spring bones), or hang as the animation has them
            const std::string v = args.size() > 2 ? args[2] : "";
            if (v == "on" || v == "off" || v == "toggle")
                m_anim.setPhysics(v == "on" || (v == "toggle" && !m_anim.physics()));
            else if (!v.empty())
                return "error: avatar physics [on|off|toggle]";
            return m_anim.physics() ? "on" : "off";
        }
        return "";
    }

    // avatar emote [name|number|file|folder [once|loop]|stop]
    std::string CAvatarControl::emote(const std::vector<std::string>& args, std::string rest,
                                      const std::function<std::string(const std::string&, int)>& loadEmote) {
        if (!avatar)
            return noAvatar();
        const auto& a   = *avatar;
        const auto& all = m_anim.emotes();
        if (args.size() < 3) {
            std::string list;
            for (size_t i = 0; i < all.size(); ++i)
                list += std::format(R"({}{{"name": "{}", "from": "{}", "loop": {}, "hold": {}, "duration": {:.2f}, "speed": {:.3f}}})", i ? ", " : "",
                                    jsonEscape(all[i]->name), jsonEscape(all[i]->from), all[i]->loop, all[i]->hold, all[i]->anim.duration, all[i]->speed);
            const int e = m_anim.emote();
            return std::format(R"({{"playing": "{}", "loading": {}, "emotes": [{}]}})", e >= 0 ? jsonEscape(all[e]->name) : "", emotesLoading, list);
        }
        if (args.size() == 3 && (args[2] == "stop" || args[2] == "none" || args[2] == "off")) {
            m_anim.stopEmote();
            return "ok";
        }
        // once or over and over, else as the emote has it
        int loop = -1;
        if (args.size() > 3 && (args.back() == "once" || args.back() == "loop")) {
            loop = args.back() == "loop";
            rest.resize(rest.find_last_not_of(" \t\n", rest.rfind(args.back()) - 1) + 1);
        }
        rest = unquote(rest);
        if (rest.find('/') == std::string::npos && !isEmoteFile(rest)) {
            const int e = m_anim.findEmote(rest);
            if (e < 0)
                return std::format("error: {} has no emote \"{}\" (hyprctl hypr3d avatar emote lists them)", a.name, rest);
            m_anim.playEmote(e, loop);
            return all[e]->name;
        }
        std::error_code ec;
        const auto      abs = std::filesystem::absolute(rest, ec);
        if (ec || !std::filesystem::exists(abs, ec))
            return "error: no such file or folder: " + rest;
        return loadEmote ? loadEmote(abs.string(), loop) : "error: emote files can't be loaded here";
    }

    // avatar expression [name [weight]|none], avatar gesture [left|right|both <gesture>]
    std::string CAvatarControl::face(const std::vector<std::string>& args) {
        if (!avatar)
            return noAvatar();
        const auto& a = *avatar;

        if (args[1] == "gesture") {
            if (args.size() < 3)
                return std::format(R"({{"left": "{}", "right": "{}"}})", gestureName(m_anim.gesture(0)), gestureName(m_anim.gesture(1)));
            const std::string& hand = args[2];
            if (hand != "left" && hand != "right" && hand != "both")
                return "error: which hand: left, right or both";
            const int g = args.size() > 3 ? gestureFromName(args[3]) : (int)GESTURE_NEUTRAL;
            if (g < 0)
                return "error: the gestures are neutral, fist, open, point, victory, rocknroll, handgun and thumbsup (or 0-7)";
            if (hand != "right")
                m_anim.setGesture(0, g);
            if (hand != "left")
                m_anim.setGesture(1, g);
            const int face = a.gestureFace[hand == "left" ? 0 : 1][g];
            return face >= 0 ? std::format("{} ({})", gestureName(g), a.expressions[face].name) : gestureName(g);
        }

        if (args.size() < 3) {
            std::string own, keys, presets;
            for (const auto& e : a.expressions) {
                std::string& list = e.shapeKey ? keys : own;
                list += std::format(R"({}"{}")", list.empty() ? "" : ", ", jsonEscape(e.name));
            }
            for (int p = 0; p < EX_COUNT; ++p)
                if (a.preset[p] >= 0)
                    presets += std::format(R"({}"{}": "{}")", presets.empty() ? "" : ", ", expressionPresetName(p), jsonEscape(a.expressions[a.preset[p]].name));
            const int held = m_anim.expression();
            return std::format(R"({{"expression": "{}", "weight": {:.2f}, "from": "{}", "presets": {{{}}}, "expressions": [{}], "shapeKeys": [{}]}})",
                               held >= 0 ? jsonEscape(a.expressions[held].name) : "", expressionWeight, jsonEscape(a.expressionsFrom), presets, own, keys);
        }
        // the name can have spaces; a number at the end is the weight
        std::vector<std::string> words(args.begin() + 2, args.end());
        float                    weight = 1;
        if (words.size() > 1) {
            char*       end = nullptr;
            const float w   = std::strtof(words.back().c_str(), &end);
            if (end && *end == 0) {
                weight = std::clamp(w, 0.f, 1.f);
                words.pop_back();
            }
        }
        std::string name;
        for (const auto& w : words)
            name += (name.empty() ? "" : " ") + w;
        if (name == "none" || name == "off" || name == "clear") {
            expression.clear();
            m_anim.setExpression(-1);
            return "ok";
        }
        const int e = a.findExpression(name);
        if (e < 0)
            return std::format("error: {} has no expression \"{}\" (hyprctl hypr3d avatar expression lists them)", a.name, name);
        expression       = name;
        expressionWeight = weight;
        m_anim.setExpression(e, weight);
        return a.expressions[e].name;
    }

    std::string CAvatarControl::setOutfit(const std::string& name, int& state) {
        const auto& a = *avatar;
        if (const int t = a.findToggle(name); t >= 0) {
            const bool on = state == 2 ? !m_anim.toggle(t) : state < 0 ? a.toggles[t].on : state == 1;
            if (state == 2)
                state = on;
            m_anim.setToggle(t, on);
            return std::format("{}: {}", a.toggles[t].name, on ? "on" : "off");
        }
        const std::vector<int> parts = a.findParts(name);
        if (parts.empty())
            return std::format("error: {} has no toggle or part \"{}\" (hyprctl hypr3d avatar parts lists them)", a.name, name);
        if (state == 2)
            state = !m_anim.partsShown()[parts[0]];
        for (const int p : parts)
            m_anim.setPart(p, state);
        return std::format("{}: {}{}", parts.size() == 1 ? a.parts[parts[0]].name : std::format("{} ({} parts)", name, parts.size()),
                           m_anim.partsShown()[parts[0]] ? "shown" : "hidden", state < 0 ? " (as the toggles have it)" : "");
    }

    // a slider set by hand (NaN: back where the settings file starts it), kept for when the avatar is loaded again
    void CAvatarControl::changeSlider(const std::string& name, float value, float valueY) {
        if (!avatar)
            return;
        const int s = avatar->findSlider(name);
        if (s < 0)
            return;
        m_anim.setSlider(s, value, valueY);
        std::erase_if(slidersSet, [&](const auto& o) { return gltf::lower(std::get<0>(o)) == gltf::lower(name); });
        if (!std::isnan(value))
            slidersSet.emplace_back(name, m_anim.slider(s), m_anim.sliderY(s));
    }

    std::string CAvatarControl::changeOutfit(const std::string& name, int state) {
        const std::string r = setOutfit(name, state);
        if (!r.starts_with("error")) { // for when the avatar is loaded again
            const std::string low = gltf::lower(name);
            std::erase_if(outfitSet, [&](const auto& o) { return o.first == low; });
            outfitSet.emplace_back(low, state);
        }
        return r;
    }

    void CAvatarControl::resetOutfit() {
        outfitSet.clear();
        shapesSet.clear();
        slidersSet.clear();
        m_anim.resetOutfit();
    }

    // avatar parts [reset], avatar toggle <name> [on|off|reset], avatar shape <shape key> [weight|reset],
    // avatar slider <name> [0..1|NN%|reset], a 2D one <name> [x y (-1..1 or NN%)|reset]
    std::string CAvatarControl::outfit(const std::vector<std::string>& args) {
        if (!avatar)
            return noAvatar();
        const auto&              a = *avatar;
        std::vector<std::string> words(args.begin() + 2, args.end());
        auto                     joined = [&] {
            std::string name;
            for (const auto& w : words)
                name += (name.empty() ? "" : " ") + w;
            return name;
        };

        if (args[1] == "parts") {
            if (!words.empty() && words[0] == "reset") {
                resetOutfit();
                return "ok";
            }
            std::string toggles, sliders, parts, shapes, variants;
            for (size_t t = 0; t < a.toggles.size(); ++t)
                toggles += std::format(R"({}{{"name": "{}", "group": "{}", "on": {}}})", t ? ", " : "", jsonEscape(a.toggles[t].name), jsonEscape(a.toggles[t].group),
                                       m_anim.toggle((int)t));
            for (size_t s = 0; s < a.sliders.size(); ++s)
                sliders += a.sliders[s].grid ? std::format(R"({}{{"name": "{}", "value": [{:.3f}, {:.3f}]}})", s ? ", " : "", jsonEscape(a.sliders[s].name),
                                                           m_anim.slider((int)s), m_anim.sliderY((int)s))
                                             : std::format(R"({}{{"name": "{}", "value": {:.3f}}})", s ? ", " : "", jsonEscape(a.sliders[s].name), m_anim.slider((int)s));
            for (size_t v = 0; v < a.variants.size(); ++v)
                variants += std::format(R"({}{{"name": "{}", "on": {}}})", v ? ", " : "", jsonEscape(a.variants[v]), m_anim.variant((int)v));
            const auto& shown = m_anim.partsShown();
            for (size_t p = 0; p < a.parts.size(); ++p)
                parts += std::format(R"({}{{"name": "{}", "shown": {}, "triangles": {}}})", p ? ", " : "", jsonEscape(a.parts[p].name), p < shown.size() && shown[p],
                                     a.parts[p].triangles);
            for (const auto& [name, w] : shapesSet)
                shapes += std::format(R"({}"{}": {:.2f})", shapes.empty() ? "" : ", ", jsonEscape(name), w);
            return std::format(R"({{"settings": "{}", "toggles": [{}], "sliders": [{}], "variants": [{}], "parts": [{}], "shapes": {{{}}}}})", jsonEscape(a.settings),
                               toggles, sliders, variants, parts, shapes);
        }

        if (args[1] == "slider") {
            // the numbers at the end (up to two), the rest the name
            auto number = [](std::string w, float& out) {
                const bool pct = !w.empty() && w.back() == '%';
                if (pct)
                    w.pop_back();
                char*       end = nullptr;
                const float x   = std::strtof(w.c_str(), &end);
                if (!end || end == w.c_str() || *end != 0)
                    return false;
                out = pct ? x / 100.f : x;
                return true;
            };
            const bool reset = !words.empty() && (words.back() == "reset" || words.back() == "default");
            if (reset)
                words.pop_back();
            float nums[2] = {NAN, NAN};
            int   have    = 0;
            for (size_t k = words.size(); k-- > 0 && have < 2;)
                if (float x; number(words[k], x))
                    ++have;
                else
                    break;
            for (int k = reset ? 0 : have; k >= 0; --k) {
                std::string name;
                for (size_t i = 0; i + k < words.size(); ++i)
                    name += (i ? " " : "") + words[i];
                const int s = a.findSlider(name);
                if (s < 0 || (k == 2 && !a.sliders[s].grid) || (k == 1 && a.sliders[s].grid))
                    continue;
                for (int i = 0; i < k; ++i)
                    number(words[words.size() - k + i], nums[i]);
                if (reset)
                    changeSlider(a.sliders[s].name, NAN);
                else if (k == 2)
                    changeSlider(a.sliders[s].name, nums[0], nums[1]);
                else if (k == 1)
                    changeSlider(a.sliders[s].name, std::clamp(nums[0], 0.f, 1.f));
                if (a.sliders[s].grid)
                    return std::format("{}: {:+.0f}% {:+.0f}%", a.sliders[s].name, m_anim.slider(s) * 100, m_anim.sliderY(s) * 100);
                return std::format("{}: {:.0f}%", a.sliders[s].name, m_anim.slider(s) * 100);
            }
            return std::format("error: {} has no slider \"{}\" (hyprctl hypr3d avatar parts lists them; a 2D one takes x and y)", a.name, joined());
        }

        if (args[1] == "toggle") {
            static constexpr std::pair<std::string_view, int> STATES[] = {{"on", 1},     {"show", 1},  {"shown", 1},   {"off", 0},    {"hide", 0},
                                                                          {"hidden", 0}, {"reset", -1}, {"default", -1}, {"toggle", 2}, {"flip", 2}};
            int state = 2;
            if (words.size() > 1)
                for (const auto& [word, st] : STATES)
                    if (words.back() == word) {
                        state = st;
                        words.pop_back();
                        break;
                    }
            const std::string name = joined();
            if (name.empty())
                return "error: toggle what (hyprctl hypr3d avatar parts lists them)";
            return changeOutfit(name, state);
        }

        // shape: the name can have spaces (and "mesh/"); a number at the end is the weight
        float w   = NAN;
        bool  set = false;
        if (words.size() > 1) {
            char*       end = nullptr;
            const float v   = std::strtof(words.back().c_str(), &end);
            if (words.back() == "reset" || words.back() == "default")
                set = true;
            else if (end && end != words.back().c_str() && *end == 0) {
                w   = std::clamp(v, 0.f, 1.f);
                set = true;
            }
            if (set)
                words.pop_back();
        }
        const std::string name = joined();
        if (name.empty())
            return "error: which shape key (hyprctl hypr3d avatar expression lists them)";
        const std::vector<int> morphs = a.findMorphs(name);
        if (morphs.empty())
            return std::format("error: {} has no shape key \"{}\" (hyprctl hypr3d avatar expression lists them)", a.name, name);
        if (set) {
            for (const int m : morphs)
                m_anim.setShape(m, w);
            std::erase_if(shapesSet, [&](const auto& o) { return gltf::lower(o.first) == gltf::lower(name); });
            if (!std::isnan(w))
                shapesSet.emplace_back(name, w);
        }
        return std::format("{}: {:.2f}{}", name, m_anim.shape(morphs[0]), morphs.size() > 1 ? std::format(" ({} shape keys)", morphs.size()) : "");
    }

    std::string CAvatarControl::action(const SMenuItem& it) {
        switch (it.action) {
            case MA_EMOTE:
            case MA_EMOTE_STOP:
            case MA_EXPRESSION:
            case MA_GESTURE:
            case MA_TOGGLE:
            case MA_PART:
            case MA_OUTFIT_RESET:
            case MA_SLIDER:
            case MA_PHYSICS:
            case MA_FACE_RESET: break;
            default: return "";
        }
        if (!avatar)
            return noAvatar();
        const auto& a = *avatar;
        switch (it.action) {
            case MA_EMOTE: // again: it stops
                if (it.arg < 0 || it.arg >= (int)m_anim.emotes().size())
                    return "error: no such emote";
                if (m_anim.emote() == it.arg) {
                    m_anim.stopEmote();
                    return "stopped";
                }
                m_anim.playEmote(it.arg);
                return m_anim.emotes()[it.arg]->name;
            case MA_EMOTE_STOP: m_anim.stopEmote(); return "ok";
            case MA_EXPRESSION: { // held till it's picked again
                if (it.arg < 0 || it.arg >= (int)a.expressions.size())
                    return "error: no such expression";
                if (m_anim.expression() == it.arg) {
                    expression.clear();
                    m_anim.setExpression(-1);
                    return "none";
                }
                const SExpression& x = a.expressions[it.arg];
                expression           = x.preset >= 0 ? expressionPresetName(x.preset) : x.name; // a preset for the next avatar too
                expressionWeight     = 1;
                m_anim.setExpression(it.arg, 1);
                return x.name;
            }
            case MA_GESTURE: {
                const int g = std::clamp(it.arg2, 0, GESTURE_COUNT - 1);
                if (it.arg != 1)
                    m_anim.setGesture(0, g);
                if (it.arg != 0)
                    m_anim.setGesture(1, g);
                return gestureName(g);
            }
            case MA_TOGGLE:
                if (it.arg < 0 || it.arg >= (int)a.toggles.size())
                    return "error: no such toggle";
                return changeOutfit(a.toggles[it.arg].name, 2);
            case MA_PART: { // as the page shows it: on when any of that name is
                if (it.arg < 0 || it.arg >= (int)a.parts.size())
                    return "error: no such part";
                const std::string& name  = a.parts[it.arg].name;
                const auto&        shown = m_anim.partsShown();
                bool               on    = false;
                for (size_t p = 0; p < a.parts.size(); ++p)
                    on |= (name.empty() ? (int)p == it.arg : a.parts[p].name == name) && (p >= shown.size() || shown[p]);
                if (!name.empty() && a.findToggle(name) < 0)
                    return changeOutfit(name, on ? 0 : 1); // and again when it's loaded again
                // unnamed, or a toggle has its name (and "avatar toggle" would find that): just the parts
                for (const int p : name.empty() ? std::vector<int>{it.arg} : a.findParts(name))
                    m_anim.setPart(p, on ? 0 : 1);
                return on ? "hidden" : "shown";
            }
            case MA_OUTFIT_RESET: resetOutfit(); return "ok";
            case MA_SLIDER: return "ok"; // the menu opens its dial
            case MA_PHYSICS: m_anim.setPhysics(!m_anim.physics()); return m_anim.physics() ? "on" : "off";
            case MA_FACE_RESET:
                expression.clear();
                m_anim.setExpression(-1);
                m_anim.setGesture(0, GESTURE_NEUTRAL);
                m_anim.setGesture(1, GESTURE_NEUTRAL);
                return "ok";
            default: return "error: nothing to do";
        }
    }

    void CAvatarControl::dial(const SMenuItem& it, float value, float value2) {
        if (it.action == MA_SLIDER && avatar && it.arg >= 0 && it.arg < (int)avatar->sliders.size())
            changeSlider(avatar->sliders[it.arg].name, value, it.axes == 2 ? value2 : NAN);
    }

    bool menuKey(CActionMenu& menu, uint32_t k, const FMenuPick& pick) {
        if (!menu.open())
            return false;
        switch (k) {
            case K_ESC: menu.hide(); return true;
            case K_BACKSPACE: menu.back(); return true;
            case K_ENTER:
            case K_KPENTER: pick(menu.pick()); return true;
            case K_E:
            case K_G:
            case K_X: return true; // they're for what the crosshair points at, and it's hidden
            default:
                if (k >= K_1 && k <= K_8) {
                    pick(menu.pick((int)(k - K_1)));
                    return true;
                }
                return false; // walking still works
        }
    }

    bool menuButton(CActionMenu& menu, uint32_t button, const FMenuPick& pick) {
        if (!menu.open())
            return false;
        if (button == BTN_LEFT_)
            pick(menu.pick());
        else if (button == BTN_RIGHT_)
            menu.back();
        else if (button == BTN_MIDDLE_)
            menu.hide();
        return true;
    }

    void menuWheel(CActionMenu& menu, float& fraction, float notches) {
        if (std::signbit(notches) != std::signbit(fraction))
            fraction = 0;
        fraction += notches;
        const int steps = (int)fraction;
        fraction -= steps;
        menu.scroll(steps);
    }

    std::string menuStatus(const CActionMenu& menu) {
        if (!menu.open())
            return R"({"open": false})";
        const SMenuPage& p = menu.page();
        std::string      items;
        for (size_t i = 0; i < p.items.size(); ++i) {
            const auto& it = p.items[i];
            items += std::format(R"({}{{"slot": {}, "label": "{}", "hint": "{}", "on": {}, "disabled": {}, "submenu": {}}})", i ? ", " : "", i + 1, jsonEscape(it.label),
                                 jsonEscape(it.hint), it.on, it.disabled, !it.page.empty());
        }
        // the slot the cursor points at, 0 = the middle, -1 = nothing
        const int   h    = menu.highlighted();
        std::string dial = "null"; // a slider's dial over the page
        if (const SMenuItem* d = menu.dial())
            dial = d->axes == 2 ? std::format(R"({{"label": "{}", "value": [{:.3f}, {:.3f}]}})", jsonEscape(d->label), d->value, d->value2)
                                : std::format(R"({{"label": "{}", "value": {:.3f}}})", jsonEscape(d->label), d->value);
        return std::format(R"({{"open": true, "path": "{}", "title": "{}", "highlight": {}, "items": [{}], "dial": {}}})", jsonEscape(menu.path()), jsonEscape(p.title),
                           h >= 0 ? h + 1 : h == -1 ? 0 : -1, items, dial);
    }

    std::string menuCommand(CActionMenu& menu, const std::vector<std::string>& args, const std::function<std::string(const SMenuItem&)>& action) {
        if (args.empty())
            return menuStatus(menu);
        const std::string& verb = args[0];
        auto               num  = [&](size_t i, float def) {
            try {
                return i < args.size() ? std::stof(args[i]) : def;
            } catch (...) { return def; }
        };
        auto where = [&] { return menu.open() ? menu.path() : std::string("closed"); };

        if (verb == "open" || verb == "toggle") {
            if (verb == "toggle" && menu.open()) {
                menu.hide();
                return "closed";
            }
            const std::string page = args.size() > 1 ? args[1] : CActionMenu::ROOT;
            if (!menu.show(page))
                return "error: no such page: " + page + " (main, emotes, expressions, gestures, left, right, both, outfit, parts, options; or a path: gestures/left)";
            return where();
        }
        if (verb == "close") {
            menu.hide();
            return "closed";
        }
        if (verb != "back" && verb != "pick" && verb != "move" && verb != "scroll")
            return "error: menu [open [page]|close|toggle|back|pick [n]|move dx dy|scroll n]";
        if (!menu.open())
            return "error: the menu isn't open (hyprctl hypr3d menu open)";
        if (verb == "back") {
            menu.back();
            return where();
        }
        if (verb == "pick") { // pick n: slot n, 0 = the middle; pick: what the cursor points at
            const auto& items = menu.page().items;
            int         slot  = menu.highlighted();
            if (args.size() > 1) {
                const float n = num(1, -1);
                if (n < 0 || n > items.size() || n != std::floor(n))
                    return std::format("error: pick 1-{}, or 0 for the middle", items.size());
                slot = (int)n - 1;
            }
            if (slot == -2)
                return "error: the cursor isn't on anything";
            if (slot >= 0 && items[slot].disabled)
                return std::format("error: {} can't be picked{}", items[slot].label, items[slot].hint.empty() ? "" : " (" + items[slot].hint + ")");
            const auto item = menu.pick(slot);
            return item ? action(*item) : where();
        }
        if (verb == "move") { // logical pixels, as the mouse moves it
            menu.move(num(1, 0), num(2, 0));
            return "ok";
        }
        menu.scroll((int)num(1, 1));
        return "ok";
    }
}
