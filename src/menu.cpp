#include "menu.hpp"

#include "gltf.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <numbers>
#include <span>

#include <cairo.h>
#include <pango/pangocairo.h>

namespace h3d {

    namespace {
        constexpr float HALF_TURN = std::numbers::pi_v<float>, TAU = 2 * HALF_TURN; // (not PI: Hyprland has a macro of that name)
        constexpr float INNER      = 0.34f; // the middle's radius, in the ring's
        constexpr float CURSOR_MAX = 0.97f; // how far out the cursor goes, in the ring's radius
        constexpr float OPEN_TIME = 0.12f, CLOSE_TIME = 0.1f, FLASH_TIME = 0.25f; // seconds
        constexpr auto  MORE = "\x01more"; // the page of the "More" slot: the next part of this one

        float           cursorRadius(int R) {
            return std::max(4.f, 0.026f * R);
        }

        struct SColor {
            double r, g, b;
        };
        constexpr SColor WHITE{1, 1, 1}, ACCENT{0.33, 0.78, 1.0}, HINT{0.72, 0.82, 0.92};

        struct SUnref {
            void operator()(void* p) const {
                g_object_unref(p);
            }
        };

        // a block of text, laid out
        struct SBlock {
            std::unique_ptr<PangoLayout, SUnref> layout; // null = none
            double                               width = 0, height = 0;
            PangoRectangle                       ink{}; // where it draws, from its top left
        };

        // in lines `width` wide at most, centered; none when it's empty, or when `whole` and the font lacks some of
        // it (an emoji it doesn't have)
        SBlock block(PangoContext* ctx, std::string_view text, const char* family, double px, bool bold, double width, int lines, bool whole = false) {
            SBlock b;
            if (text.empty())
                return b;
            PangoLayout*          l  = pango_layout_new(ctx);
            PangoFontDescription* fd = pango_font_description_new();
            pango_font_description_set_family(fd, family);
            pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
            pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
            pango_layout_set_font_description(l, fd);
            pango_font_description_free(fd);
            pango_layout_set_width(l, (int)std::max(1.0, width * PANGO_SCALE));
            pango_layout_set_height(l, -std::max(lines, 1));
            pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
            pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
            pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
            pango_layout_set_text(l, text.data(), (int)text.size());
            if (whole && pango_layout_get_unknown_glyphs_count(l) > 0) {
                g_object_unref(l);
                return b;
            }
            PangoRectangle logical;
            pango_layout_get_pixel_extents(l, &b.ink, &logical);
            b.layout.reset(l);
            b.width  = width;
            b.height = logical.height;
            return b;
        }

        // centered on x, its top at y
        void paint(cairo_t* cr, const SBlock& b, double x, double y, SColor c, double a, bool emoji = false) {
            if (!b.layout || a <= 0)
                return;
            x -= b.width / 2;
            if (!emoji || a >= 1) {
                cairo_set_source_rgba(cr, c.r, c.g, c.b, std::min(a, 1.0));
                cairo_move_to(cr, x, y);
                pango_cairo_show_layout(cr, b.layout.get());
                return;
            }
            // color emoji don't take the source's alpha: fade it as a whole
            cairo_save(cr);
            cairo_rectangle(cr, std::floor(x + b.ink.x) - 1, std::floor(y + b.ink.y) - 1, b.ink.width + 3, b.ink.height + 3);
            cairo_clip(cr);
            cairo_push_group(cr);
            cairo_set_source_rgb(cr, c.r, c.g, c.b);
            cairo_move_to(cr, x, y);
            pango_cairo_show_layout(cr, b.layout.get());
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, a);
            cairo_restore(cr);
        }
    }

    // --- the menu

    bool CActionMenu::show(const std::string& page) {
        // a path, as path() has it: "gestures/left", "main/emotes:2"
        std::vector<SLevel> stack{{ROOT}};
        for (size_t from = 0; from <= page.size();) {
            const size_t end  = std::min(page.find('/', from), page.size());
            std::string  part = page.substr(from, end - from);
            from              = end + 1;
            if (part.empty())
                continue;
            int chunk = 0;
            if (const size_t c = part.find(':'); c != std::string::npos) {
                chunk = std::max(0, std::atoi(part.c_str() + c + 1) - 1);
                part.resize(c);
            }
            if (m_pages(part).title.empty())
                return false;
            if (part == ROOT && stack.size() == 1)
                stack[0].chunk = chunk;
            else
                stack.push_back({part, chunk});
        }
        m_stack = std::move(stack);
        m_cx = m_cy = 0;
        m_flash     = 0;
        refresh();
        return true;
    }

    void CActionMenu::hide() {
        m_stack.clear();
    }

    void CActionMenu::back() {
        if (m_stack.empty())
            return;
        if (m_stack.back().chunk > 0)
            --m_stack.back().chunk;
        else
            m_stack.pop_back();
        m_cx = m_cy = 0;
        m_flash     = 0;
        if (!m_stack.empty())
            refresh();
    }

    void CActionMenu::move(float dx, float dy) {
        if (!open())
            return;
        m_cx += dx / m_radiusLogical;
        m_cy += dy / m_radiusLogical;
        if (const float r = std::hypot(m_cx, m_cy); r > CURSOR_MAX) {
            m_cx *= CURSOR_MAX / r;
            m_cy *= CURSOR_MAX / r;
        }
    }

    int CActionMenu::highlighted() const {
        if (!open())
            return -2;
        if (std::hypot(m_cx, m_cy) < INNER)
            return -1;
        const int n = (int)m_page.items.size();
        if (n == 0)
            return -2;
        float a = std::atan2(m_cx, -m_cy); // clockwise from the top
        if (a < 0)
            a += TAU;
        return (int)std::floor(a / (TAU / n) + 0.5f) % n;
    }

    void CActionMenu::aimAt(int slot) {
        const int n = (int)m_page.items.size();
        if (slot < 0 || slot >= n) {
            m_cx = m_cy = 0;
            return;
        }
        const float t = slot * TAU / n, r = INNER + 0.08f; // inside, not over its text
        m_cx          = r * std::sin(t);
        m_cy          = -r * std::cos(t);
    }

    void CActionMenu::scroll(int steps) {
        const int n = (int)m_page.items.size();
        if (!open() || n == 0 || steps == 0 || std::ranges::all_of(m_page.items, &SMenuItem::disabled))
            return;
        int s = highlighted();
        if (s < 0)
            s = steps > 0 ? -1 : n;
        const int dir = steps > 0 ? 1 : -1;
        for (int k = std::abs(steps); k > 0; --k) {
            do
                s = ((s + dir) % n + n) % n;
            while (m_page.items[s].disabled);
        }
        aimAt(s);
    }

    std::optional<SMenuItem> CActionMenu::pick() {
        if (!open())
            return std::nullopt;
        refresh();
        return pick(highlighted());
    }

    std::optional<SMenuItem> CActionMenu::pick(int slot) {
        if (!open())
            return std::nullopt;
        refresh();
        if (slot == -1) {
            back();
            return std::nullopt;
        }
        if (slot < 0 || slot >= (int)m_page.items.size() || m_page.items[slot].disabled)
            return std::nullopt;
        const SMenuItem item = m_page.items[slot];
        if (item.page == MORE)
            ++m_stack.back().chunk;
        else if (!item.page.empty()) {
            if (m_pages(item.page).title.empty())
                return std::nullopt;
            m_stack.push_back({item.page});
        } else {
            // it stays open: the cursor goes to it (a key picked it), and it lights up
            if (slot != highlighted())
                aimAt(slot);
            m_flash     = 1;
            m_flashSlot = slot;
            return item;
        }
        m_cx = m_cy = 0;
        m_flash     = 0;
        refresh();
        return std::nullopt;
    }

    void CActionMenu::refresh() {
        // a page that's gone (the owner doesn't have it any more) goes back
        SMenuPage page;
        while (!m_stack.empty() && (page = m_pages(m_stack.back().id)).title.empty())
            m_stack.pop_back();
        if (m_stack.empty()) {
            m_page = {};
            return;
        }
        SLevel&   top = m_stack.back();
        const int n   = (int)page.items.size();
        if (n <= SLOTS) {
            top.chunk = 0;
            m_page    = std::move(page);
            return;
        }
        // seven at a time and "More", the last part up to eight
        const int per = SLOTS - 1, chunks = (n - 2) / per + 1;
        top.chunk        = std::clamp(top.chunk, 0, chunks - 1);
        const int  from  = top.chunk * per;
        const bool last  = top.chunk == chunks - 1;
        const int  count = last ? n - from : per;
        SMenuPage  out{.title = std::format("{} {}/{}", page.title, top.chunk + 1, chunks)};
        out.items.assign(page.items.begin() + from, page.items.begin() + from + count);
        if (!last)
            out.items.push_back({.label = "More", .hint = std::format("{} more", n - from - count), .icon = "➡️", .page = MORE});
        m_page = std::move(out);
    }

    std::string CActionMenu::path() const {
        std::string out;
        for (const SLevel& l : m_stack) {
            if (!out.empty())
                out += '/';
            out += l.id;
            if (l.chunk > 0)
                out += std::format(":{}", l.chunk + 1);
        }
        return out;
    }

    void CActionMenu::update(float dt, int outW, int outH, float scale) {
        dt      = std::max(dt, 0.f);
        m_fade  = open() ? std::min(1.f, m_fade + dt / OPEN_TIME) : std::max(0.f, m_fade - dt / CLOSE_TIME);
        m_flash = std::max(0.f, m_flash - dt / FLASH_TIME);
        if (!visible() || outW < 1 || outH < 1)
            return;

        // right of the middle, where it doesn't hide the avatar in third person
        const int side  = std::min(outW, outH);
        const int R     = std::max(40, (int)std::lround(0.28f * side));
        m_radiusLogical = R / std::max(scale, 0.1f);
        m_x             = outW / 2.f + std::max(0.f, std::min(outW * 0.22f, outW / 2.f - R - 0.04f * side));
        m_y             = outH / 2.f;

        if (open())
            refresh();
        // closing, it fades out as it was
        const int highlight = open() ? highlighted() : m_drawnHighlight;
        const int flash     = open() && m_flash > 0 ? (int)std::ceil(m_flash * 4) : 0;
        if (R != m_drawnR || highlight != m_drawnHighlight || flash != m_drawnFlash || m_page != m_drawnPage)
            draw(R, highlight, flash);
    }

    SHudImage CActionMenu::hud() const {
        SHudImage h;
        if (!visible() || m_pixels.empty())
            return h;
        h.pixels      = &m_pixels;
        h.w = h.h     = m_size;
        h.serial      = m_serial;
        h.x           = m_x;
        h.y           = m_y;
        const float e = m_fade * m_fade * (3 - 2 * m_fade);
        h.alpha       = e;
        h.scale       = 0.92f + 0.08f * e; // it pops open
        if (open()) {
            const float c = m_size / 2.f;
            h.cursor[0]   = c + m_cx * m_drawnR;
            h.cursor[1]   = c + m_cy * m_drawnR;
            h.cursor[2]   = cursorRadius(m_drawnR);
        }
        return h;
    }

    void CActionMenu::draw(int R, int highlight, int flash) {
        const int    M = (int)std::ceil(0.12f * R), size = 2 * (R + M); // M: room for the shadow
        const double C = R + M, Ro = R, Ri = INNER * R, gap = std::max(2.0, 0.02 * R), Rin = Ri + gap;
        const int    n = (int)m_page.items.size();

        m_pixels.assign((size_t)size * size, 0);
        cairo_surface_t* surface = cairo_image_surface_create_for_data((unsigned char*)m_pixels.data(), CAIRO_FORMAT_ARGB32, size, size, size * 4);
        cairo_t*         cr      = cairo_create(surface);
        {
            std::unique_ptr<PangoContext, SUnref> ctx(pango_cairo_create_context(cr));
            cairo_font_options_t*                 fo = cairo_font_options_create();
            cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY); // not subpixel: it's see-through
            cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_SLIGHT);
            pango_cairo_context_set_font_options(ctx.get(), fo);
            cairo_font_options_destroy(fo);

            // a soft shadow under it all, so it reads over anything
            cairo_pattern_t* shadow = cairo_pattern_create_radial(C, C, Ro * 0.97, C, C, Ro + M);
            cairo_pattern_add_color_stop_rgba(shadow, 0, 0, 0, 0, 0.38);
            cairo_pattern_add_color_stop_rgba(shadow, 0.35, 0, 0, 0, 0.16);
            cairo_pattern_add_color_stop_rgba(shadow, 1, 0, 0, 0, 0);
            cairo_pattern_set_extend(shadow, CAIRO_EXTEND_PAD);
            cairo_set_source(cr, shadow);
            cairo_arc(cr, C, C, Ro + M, 0, TAU);
            cairo_fill(cr);
            cairo_pattern_destroy(shadow);

            // the wedges, clockwise from the top, with gaps as wide all the way out
            auto wedge = [&](int i) {
                cairo_new_path(cr);
                if (n <= 1) {
                    cairo_arc(cr, C, C, Ro, 0, TAU);
                    cairo_close_path(cr);
                    cairo_new_sub_path(cr);
                    cairo_arc_negative(cr, C, C, Rin, TAU, 0);
                    cairo_close_path(cr);
                    return;
                }
                const double mid = i * TAU / n - HALF_TURN / 2, span = HALF_TURN / n;
                const double ao = std::asin(gap / 2 / Ro), ai = std::asin(gap / 2 / Rin);
                cairo_arc(cr, C, C, Ro, mid - span + ao, mid + span - ao);
                cairo_arc_negative(cr, C, C, Rin, mid + span - ai, mid - span + ai);
                cairo_close_path(cr);
            };
            for (int i = 0; i < std::max(n, 1); ++i) {
                const SMenuItem* item  = i < n ? &m_page.items[i] : nullptr;
                const bool       hover = item && i == highlight && !item->disabled, on = item && item->on;
                wedge(i);
                cairo_set_source_rgba(cr, 0.07, 0.08, 0.10, 0.85);
                cairo_fill_preserve(cr);
                if (on) {
                    cairo_set_source_rgba(cr, ACCENT.r, ACCENT.g, ACCENT.b, 0.13);
                    cairo_fill_preserve(cr);
                }
                if (hover) {
                    cairo_set_source_rgba(cr, ACCENT.r, ACCENT.g, ACCENT.b, 0.34);
                    cairo_fill_preserve(cr);
                }
                if (flash > 0 && i == m_flashSlot) {
                    cairo_set_source_rgba(cr, 1, 1, 1, 0.25 * flash / 4);
                    cairo_fill_preserve(cr);
                }
                // a rim inside it: thick on what's on, thin where the cursor is
                if (on || hover) {
                    cairo_save(cr);
                    cairo_clip_preserve(cr);
                    cairo_set_line_width(cr, 2 * (on ? 0.035 : 0.012) * R);
                    cairo_set_source_rgba(cr, ACCENT.r, ACCENT.g, ACCENT.b, on ? 0.95 : 0.8);
                    cairo_stroke(cr);
                    cairo_restore(cr);
                }
                cairo_new_path(cr);
            }

            // the middle: where it is, and back
            cairo_arc(cr, C, C, Ri, 0, TAU);
            cairo_set_source_rgba(cr, 0.04, 0.05, 0.06, 0.90);
            cairo_fill_preserve(cr);
            if (highlight == -1) {
                cairo_set_source_rgba(cr, ACCENT.r, ACCENT.g, ACCENT.b, 0.2);
                cairo_fill_preserve(cr);
            }
            cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
            cairo_set_line_width(cr, std::max(1.0, 0.006 * R));
            cairo_stroke(cr);
            {
                const bool   root  = m_stack.size() <= 1 && (m_stack.empty() || m_stack[0].chunk == 0);
                const SBlock title = block(ctx.get(), m_page.title, "Sans", 0.072 * R, true, 1.7 * Ri, 2);
                const SBlock sub   = block(ctx.get(), root ? "× Close" : "‹ Back", "Sans", 0.055 * R, false, 1.7 * Ri, 1);
                // the cursor rests in the middle: between the two, not on them
                const double g = cursorRadius(R) + std::max(2.0, 0.014 * R);
                paint(cr, title, C, C - g - (title.ink.y + title.ink.height), WHITE, 0.96);
                paint(cr, sub, C, C + g - sub.ink.y, WHITE, 0.7);
            }

            // the items: an icon, the label and what it's set to, in the middle of the wedge
            const double rm = Rin + (Ro - Rin) * 0.52;
            for (int i = 0; i < n; ++i) {
                const SMenuItem& it    = m_page.items[i];
                const double     t     = i * TAU / n; // clockwise from the top
                const double     x     = C + rm * std::sin(t), y = C - rm * std::cos(t);
                const double     maxW  = n <= 2 ? 0.62 * R : std::min(2 * rm * std::sin(HALF_TURN / n) * 0.86, 0.62 * R);
                const double     a     = it.disabled ? 0.3 : 1;
                const SBlock     rows[] = {
                    block(ctx.get(), it.icon, "emoji", (n > 6 ? 0.13 : 0.145) * R, false, maxW, 1, true),
                    block(ctx.get(), it.label, "Sans", 0.066 * R, true, maxW, 2),
                    block(ctx.get(), it.hint, "Sans", 0.052 * R, false, maxW, 1),
                };
                const SColor colors[] = {WHITE, WHITE, HINT};
                const double alphas[] = {a, 0.96 * a, 0.7 * a};
                const double sp       = 0.012 * R;
                double       total    = -sp;
                for (const SBlock& b : rows)
                    if (b.layout)
                        total += b.height + sp;
                double ty = y - std::max(total, 0.0) / 2;
                for (size_t k = 0; k < std::size(rows); ++k)
                    if (rows[k].layout) {
                        paint(cr, rows[k], x, ty, colors[k], alphas[k], k == 0);
                        ty += rows[k].height + sp;
                    }

                // its number, for the keys
                const double na = t - std::min(HALF_TURN / n, HALF_TURN / 4) * 0.62, nr = Ro - 0.07 * R;
                const SBlock num = block(ctx.get(), std::to_string(i + 1), "Sans", 0.046 * R, true, 0.12 * R, 1);
                paint(cr, num, C + nr * std::sin(na), C - nr * std::cos(na) - num.height / 2, WHITE, 0.4 * a);
            }
            if (n == 0) {
                const SBlock b = block(ctx.get(), "Nothing here", "Sans", 0.06 * R, false, 0.6 * R, 1);
                paint(cr, b, C, C - rm - b.height / 2, WHITE, 0.5);
            }
        }
        cairo_destroy(cr);
        cairo_surface_flush(surface);
        cairo_surface_destroy(surface);

        m_size           = size;
        m_drawnR         = R;
        m_drawnHighlight = highlight;
        m_drawnFlash     = flash;
        m_drawnPage      = m_page;
        ++m_serial;
    }

    // --- the plugin's pages

    namespace {
        // "sad_kick" -> "Sad kick": the names are the avatar maker's, so no more than that
        std::string pretty(std::string_view name) {
            std::string out;
            for (char c : name) {
                if (c == '_')
                    c = ' ';
                if (c == ' ' && (out.empty() || out.back() == ' '))
                    continue;
                out += c;
            }
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            if (!out.empty() && std::islower((unsigned char)out[0]))
                out[0] = (char)std::toupper((unsigned char)out[0]);
            return out;
        }

        struct SIcon {
            std::string_view key, icon; // the first whose key is in the name
        };

        constexpr SIcon EMOTE_ICONS[] = {
            {"wave", "👋"},  {"clap", "👏"},  {"point", "👉"}, {"cheer", "🙌"}, {"dance", "💃"}, {"backflip", "🤸"}, {"flip", "🤸"}, {"sad", "😔"},
            {"die", "💀"},   {"dead", "💀"},  {"bow", "🙇"},   {"laugh", "😂"}, {"sit", "🪑"},   {"think", "🤔"},    {"shrug", "🤷"},
        };
        constexpr SIcon FACE_ICONS[] = {
            {"wink", "😉"},  {"tongue", "😛"}, {"blush", "😊"}, {"cry", "😭"},   {"heart", "😍"}, {"love", "😍"}, {"star", "🤩"},
            {"sleep", "😴"}, {"shock", "😱"},  {"smug", "😏"},  {"cat", "😺"},   {"grin", "😁"},  {"kiss", "😘"}, {"cool", "😎"},
            {"dizzy", "😵"}, {"pout", "😤"},   {"shy", "😳"},   {"evil", "😈"},  {"joy", "😄"},   {"happy", "😄"}, {"smile", "😄"},
            {"angry", "😠"}, {"sorrow", "😢"}, {"sad", "😢"},   {"fun", "😌"},   {"relax", "😌"}, {"surprise", "😮"},
        };
        constexpr std::string_view PRESET_ICONS[] = {"😄", "😠", "😢", "😌", "😮"}; // happy, angry, sad, relaxed, surprised
        // ("cape" before "cap"; not "ear", which is in "wear", or "bow")
        constexpr SIcon OUTFIT_ICONS[] = {
            {"hat", "🎩"},    {"cape", "🦸"},   {"cap", "🧢"},     {"crown", "👑"},  {"headphone", "🎧"}, {"ribbon", "🎀"}, {"flower", "🌸"},
            {"glasses", "👓"}, {"megane", "👓"}, {"mask", "🎭"},    {"hair", "💇"},   {"jacket", "🧥"},    {"coat", "🧥"},   {"hoodie", "🧥"},
            {"skirt", "👗"},  {"dress", "👗"},  {"shirt", "👕"},   {"top", "👕"},    {"pant", "👖"},      {"jean", "👖"},   {"short", "👖"},
            {"bikini", "👙"}, {"swim", "👙"},   {"glove", "🧤"},   {"scarf", "🧣"},  {"sock", "🧦"},      {"shoe", "👟"},   {"boot", "👟"},
            {"bag", "👜"},    {"tail", "🦊"},   {"ring", "💍"},    {"watch", "⌚"},
        };

        std::string iconFor(std::string_view name, std::span<const SIcon> table, std::string_view fallback) {
            const std::string low = gltf::lower(std::string(name));
            for (const auto& [key, icon] : table)
                if (low.find(key) != std::string::npos)
                    return std::string(icon);
            return std::string(fallback);
        }

        struct SGestureLook {
            std::string_view label, icon;
        };
        constexpr SGestureLook GESTURE_LOOKS[GESTURE_COUNT] = {
            {"Neutral", "🫳"}, {"Fist", "✊"}, {"Open", "🖐️"}, {"Point", "👉"}, {"Victory", "✌️"}, {"Rock 'n' roll", "🤘"}, {"Handgun", "🔫"}, {"Thumbs up", "👍"},
        };

        std::string gestureLabel(int g, bool small = false) {
            std::string s(GESTURE_LOOKS[std::clamp(g, 0, GESTURE_COUNT - 1)].label);
            if (small)
                s[0] = (char)std::tolower((unsigned char)s[0]);
            return s;
        }

        // "Fist", "L fist · R point", "" when they're both neutral
        std::string gesturesHint(const CAvatarAnimator& a) {
            const int l = a.gesture(0), r = a.gesture(1);
            if (l == r)
                return l == GESTURE_NEUTRAL ? "" : gestureLabel(l);
            std::string out;
            if (l != GESTURE_NEUTRAL)
                out = "L " + gestureLabel(l, true);
            if (r != GESTURE_NEUTRAL)
                out += (out.empty() ? "R " : " · R ") + gestureLabel(r, true);
            return out;
        }

        bool emotion(const SExpression& e) { // what the expressions page has
            return (e.preset >= EX_HAPPY && e.preset <= EX_SURPRISED) || (e.preset < 0 && !e.shapeKey);
        }

        // "blinkLeft" -> "Blink left"
        std::string presetLabel(int preset) {
            std::string out;
            for (const char c : std::string_view(expressionPresetName(preset))) {
                if (std::isupper((unsigned char)c) && !out.empty())
                    out += ' ';
                out += (char)std::tolower((unsigned char)c);
            }
            return pretty(out);
        }

        std::string expressionLabel(const SAvatarModel& m, int e) {
            if (e < 0 || e >= (int)m.expressions.size())
                return "";
            const SExpression& x = m.expressions[e];
            return x.preset >= 0 ? presetLabel(x.preset) : pretty(x.name);
        }

        // by name: those of a name go together (the unnamed ones don't), on when any of them is shown
        void addParts(SMenuPage& p, const SAvatarModel& m, const CAvatarAnimator& a) {
            const auto& shown = a.partsShown();
            for (size_t i = 0; i < m.parts.size(); ++i) {
                const std::string& name  = m.parts[i].name;
                bool               first = true, on = false;
                for (size_t j = 0; j < i && first && !name.empty(); ++j)
                    first = m.parts[j].name != name;
                if (!first)
                    continue;
                for (size_t j = i; j < m.parts.size(); ++j)
                    on |= (name.empty() ? j == i : m.parts[j].name == name) && (j >= shown.size() || shown[j]);
                p.items.push_back({.label  = name.empty() ? std::format("Part {}", i + 1) : pretty(name),
                                   .icon   = iconFor(name, OUTFIT_ICONS, ""),
                                   .action = MA_PART,
                                   .arg    = (int)i,
                                   .on     = on});
            }
        }
    }

    SMenuPage actionPage(const std::string& id, const SActionState& s) {
        const SAvatarModel*    m    = s.anim ? s.avatar : nullptr;
        const CAvatarAnimator* a    = m ? s.anim : nullptr;
        const std::string      none = s.loading ? "loading…" : "no avatar";
        SMenuPage              p;

        if (id == "main") {
            p.title = "Action Menu";
            SMenuItem emotes{.label = "Emotes", .icon = "💃", .page = "emotes"}, faces{.label = "Expressions", .icon = "😊", .page = "expressions"},
                hands{.label = "Gestures", .icon = "✌️", .page = "gestures"}, outfit{.label = "Outfit", .icon = "👕", .page = "outfit"};
            if (!a)
                for (SMenuItem* i : {&emotes, &faces, &hands, &outfit}) {
                    i->hint     = none;
                    i->disabled = true;
                }
            else {
                if (const int e = a->emote(); e >= 0 && e < (int)a->emotes().size())
                    emotes.hint = pretty(a->emotes()[e]->name);
                else if (a->emotes().empty()) {
                    emotes.hint     = "none";
                    emotes.disabled = true;
                }
                faces.hint = expressionLabel(*m, a->expression());
                if (std::ranges::none_of(m->expressions, emotion)) {
                    faces.hint     = "none";
                    faces.disabled = true;
                }
                hands.hint = gesturesHint(*a);
                if (m->toggles.empty() && m->parts.size() < 2) {
                    outfit.hint     = "nothing to change";
                    outfit.disabled = true;
                }
            }
            p.items = {emotes, faces, hands, outfit, {.label = "Options", .icon = "⚙️", .page = "options"}};
        } else if (id == "emotes") {
            p.title = "Emotes";
            for (size_t i = 0; a && i < a->emotes().size(); ++i) {
                const SAvatarEmote& e = *a->emotes()[i];
                p.items.push_back({.label  = pretty(e.name),
                                   .hint   = e.from == "built in" ? "" : e.from == "own clip" ? e.from : std::filesystem::path(e.from).filename().string(),
                                   .icon   = iconFor(e.name, EMOTE_ICONS, "🎬"),
                                   .action = MA_EMOTE,
                                   .arg    = (int)i,
                                   .on     = a->emote() == (int)i});
            }
        } else if (id == "expressions") {
            // the emotions it has (not the mouth's, the eyes'), then its own
            p.title   = "Expressions";
            auto face = [&](int e, std::string label, std::string icon) {
                p.items.push_back({.label = std::move(label), .icon = std::move(icon), .action = MA_EXPRESSION, .arg = e, .on = a->expression() == e});
            };
            for (int pr = EX_HAPPY; a && pr <= EX_SURPRISED; ++pr)
                if (const int e = m->preset[pr]; e >= 0)
                    face(e, presetLabel(pr), std::string(PRESET_ICONS[pr]));
            for (size_t e = 0; a && e < m->expressions.size(); ++e)
                if (const SExpression& x = m->expressions[e]; x.preset < 0 && !x.shapeKey)
                    face((int)e, pretty(x.name), iconFor(x.name, FACE_ICONS, "🎭"));
        } else if (id == "gestures") {
            p.title    = "Gestures";
            auto hint  = [&](int hand) { return a && a->gesture(hand) != GESTURE_NEUTRAL ? gestureLabel(a->gesture(hand)) : std::string(); };
            p.items    = {
                {.label = "Left hand", .hint = hint(0), .icon = "🤚", .page = "left", .disabled = !a},
                {.label = "Right hand", .hint = hint(1), .icon = "✋", .page = "right", .disabled = !a},
                {.label = "Both hands", .hint = a && a->gesture(0) == a->gesture(1) ? hint(0) : "", .icon = "👐", .page = "both", .disabled = !a},
            };
        } else if (id == "left" || id == "right" || id == "both") {
            // as VRChat's F1-F8 have them; the hint is the face it makes
            const int hand = id == "left" ? 0 : id == "right" ? 1 : 2;
            p.title        = hand == 0 ? "Left hand" : hand == 1 ? "Right hand" : "Both hands";
            for (int g = 0; g < GESTURE_COUNT; ++g) {
                SMenuItem it{.label = gestureLabel(g), .icon = std::string(GESTURE_LOOKS[g].icon), .action = MA_GESTURE, .arg = hand, .arg2 = g, .disabled = !a};
                if (a) {
                    int face = m->gestureFace[hand == 0 ? 0 : 1][g];
                    if (face < 0 && hand == 2)
                        face = m->gestureFace[0][g];
                    it.hint = expressionLabel(*m, face);
                    it.on   = hand == 2 ? a->gesture(0) == g && a->gesture(1) == g : a->gesture(hand) == g;
                }
                p.items.push_back(std::move(it));
            }
        } else if (id == "outfit") {
            // the settings file's toggles, and the parts by name
            p.title = "Outfit";
            if (a) {
                for (size_t i = 0; i < m->toggles.size(); ++i) {
                    const SAvatarToggle& t = m->toggles[i];
                    p.items.push_back({.label  = pretty(t.name),
                                       .hint   = t.group,
                                       .icon   = iconFor(t.name, OUTFIT_ICONS, "✨"),
                                       .action = MA_TOGGLE,
                                       .arg    = (int)i,
                                       .on     = a->toggle((int)i)});
                }
                if (m->toggles.empty())
                    addParts(p, *m, *a);
                else if (m->parts.size() > 1)
                    p.items.push_back({.label = "Parts", .icon = "🧩", .page = "parts"});
                p.items.push_back({.label = "Reset", .hint = "as it came", .icon = "🔄", .action = MA_OUTFIT_RESET});
            }
        } else if (id == "parts") {
            p.title = "Parts";
            if (a)
                addParts(p, *m, *a);
        } else if (id == "options") {
            p.title = "Options";
            p.items = {
                {.label = "View", .hint = s.third ? "third person" : "first person", .icon = "🎥", .action = MA_VIEW, .on = s.third, .disabled = !m && !s.third},
                {.label    = "Physics",
                 .hint     = !a ? none : m->springs.empty() ? "no springs" : a->physics() ? "on" : "off",
                 .icon     = "🌀",
                 .action   = MA_PHYSICS,
                 .on       = a && !m->springs.empty() && a->physics(),
                 .disabled = !a || m->springs.empty()},
                {.label = "Fly", .hint = s.fly ? "on" : "off", .icon = "🕊️", .action = MA_FLY, .on = s.fly},
                {.label = "Respawn", .icon = "📍", .action = MA_RESPAWN},
                {.label = "Reset face", .hint = "and hands", .icon = "😶", .action = MA_FACE_RESET, .disabled = !a},
                {.label = "Stop emote", .icon = "⏹️", .action = MA_EMOTE_STOP, .disabled = !a || a->emote() < 0},
            };
        }
        return p;
    }
}
