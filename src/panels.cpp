#include "panels.hpp"
#include "compat.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>

#include <algorithm>

namespace h3d {

    namespace {
        // stacking level of each kind of surface, times the layer spacing gives
        // how far in front of the wall it sits
        constexpr float DEPTH_BACKGROUND = 0.f;
        constexpr float DEPTH_BOTTOM     = 1.f;
        constexpr float DEPTH_TILED      = 2.f;
        constexpr float DEPTH_FLOATING   = 3.f;
        constexpr float DEPTH_SPECIAL    = 4.f;
        constexpr float DEPTH_TOP        = 6.f;
        constexpr float DEPTH_OVERLAY    = 7.f;
        constexpr float DEPTH_POPUP      = 0.5f; // on top of the parent
        constexpr float RANK_STEP        = 0.0005f; // meters between overlapping panels of the same level

        Vector2D uvFromViewport(const SP<CWLSurfaceResource>& s, Vector2D& br) {
            Vector2D tl{0, 0};
            br = {1, 1};
            const auto& st = s->m_current;
            if (st.viewport.hasSource && st.bufferSize.x > 0 && st.bufferSize.y > 0) {
                const auto& src = st.viewport.source;
                tl              = {src.x / st.bufferSize.x, src.y / st.bufferSize.y};
                br              = {(src.x + src.width) / st.bufferSize.x, (src.y + src.height) / st.bufferSize.y};
                if (br.x < 0.01 || br.y < 0.01) {
                    tl = {0, 0};
                    br = {1, 1};
                }
            }
            return tl;
        }

        bool drawable(const SP<CWLSurfaceResource>& s) {
            return s && s->m_current.texture && s->m_current.texture->ok() && s->m_current.size.x >= 1 && s->m_current.size.y >= 1;
        }

        // a whole surface tree (root + subsurfaces), root placed at `rootBox`
        // (which may stretch it), subsurfaces scaled along with it
        void addTree(SPanel& panel, const SP<CWLSurfaceResource>& root, const CBox& rootBox, bool cropRoot) {
            if (!root)
                return;
            const Vector2D rootSize = root->m_current.size;
            const Vector2D stretch  = cropRoot || rootSize.x < 1 || rootSize.y < 1 ? Vector2D{1, 1} : rootBox.size() / rootSize;

            root->breadthfirst(
                [&](SP<CWLSurfaceResource> s, const Vector2D& offset, void*) {
                    if (!drawable(s))
                        return;

                    SPanelSurface ps;
                    ps.tex     = s->m_current.texture;
                    ps.uvTL    = uvFromViewport(s, ps.uvBR);
                    ps.surface = s;

                    if (s == root) {
                        ps.box = rootBox;
                        if (cropRoot) {
                            // like Hyprland: show the buffer 1:1, cutting off (or
                            // extending the edge of) whatever doesn't fit the window box
                            const Vector2D ratio = rootBox.size() / rootSize;
                            ps.uvBR              = ps.uvTL + (ps.uvBR - ps.uvTL) * ratio;
                        }
                    } else {
                        ps.box = CBox{rootBox.pos() + offset * stretch, s->m_current.size * stretch};
                    }
                    panel.surfaces.emplace_back(std::move(ps));
                },
                nullptr);
        }

        void addPopups(std::vector<SPanel>& out, const SP<Desktop::View::CPopup>& head, const Vector2D& base, float parentDepth, int& order, PHLWINDOW owner,
                       float alphaMul) {
            if (!head)
                return;
            head->breadthfirst(
                [&](SP<Desktop::View::CPopup> popup, void*) {
                    if (!popup || !popup->aliveAndVisible())
                        return;
                    const auto surf = popup->resource();
                    if (!drawable(surf))
                        return;

                    SPanel p;
                    p.key    = reinterpret_cast<uintptr_t>(popup.get());
                    p.kind   = PANEL_POPUP;
                    p.window = owner;
                    p.box    = CBox{base + popup->coordsRelativeToParent(), surf->m_current.size};
                    p.depth  = parentDepth + DEPTH_POPUP;
                    p.order  = order++;
                    p.alpha  = std::clamp(hypr::alpha(popup) * alphaMul, 0.f, 1.f);
                    p.hitRoot = surf;
                    addTree(p, surf, CBox{{0, 0}, p.box.size()}, false);
                    if (!p.surfaces.empty() && p.alpha > 0.f)
                        out.emplace_back(std::move(p));
                },
                nullptr);
        }

        // `lift`: how much of a fullscreen window's hiding (Hyprland fades the top layer's surfaces that were there before
        // it to 0) is undone, 0..1
        void addLayers(std::vector<SPanel>& out, PHLMONITOR mon, int layer, float depth, int& order, float lift = 0.f) {
            for (const auto& ref : mon->m_layerSurfaceLayers[layer]) {
                const auto ls = ref.lock();
                if (!ls || (!ls->m_mapped && !hypr::fadingOut(ls)))
                    continue;
                const auto surf = ls->resource();
                if (!drawable(surf))
                    continue;

                float alpha = hypr::alpha(ls);
                if (!hypr::fadingOut(ls) && !ls->m_aboveFullscreen)
                    alpha += (1.f - alpha) * lift;
                if (alpha <= 0.f)
                    continue;

                SPanel p;
                p.key   = reinterpret_cast<uintptr_t>(ls.get());
                p.kind  = PANEL_LAYER;
                p.layer = layer;
                p.box   = CBox{hypr::realPosition(ls)->value() - mon->m_position, hypr::realSize(ls)->value()};
                p.depth = depth;
                p.order = order++;
                p.alpha = alpha;
                if (p.box.w < 1 || p.box.h < 1)
                    continue;

                p.hitRoot  = surf;
                p.hitScale = surf->m_current.size / p.box.size();
                addTree(p, surf, CBox{{0, 0}, p.box.size()}, false);
                const Vector2D popupBase = p.box.pos();
                if (!p.surfaces.empty())
                    out.emplace_back(std::move(p));

                addPopups(out, ls->m_popupHead, popupBase, depth, order, nullptr, alpha);
            }
        }

    }

    // up its WM_TRANSIENT_FOR past other menus, else the app's own window that has the keyboard, else the one it's over
    PHLWINDOW x11Owner(const PHLWINDOW& w) {
        const auto xs = w->m_xwaylandSurface.lock();
        if (!xs)
            return nullptr;
        auto up = xs->m_parent.lock();
        for (int hops = 0; up && up->m_overrideRedirect && hops < 16; ++hops)
            up = up->m_parent.lock();
        if (up)
            for (const auto& o : hypr::windows())
                if (o && o->m_isMapped && o->m_xwaylandSurface.lock() == up)
                    return o;
        const auto focus = Desktop::focusState()->window();
        const auto at    = hypr::realPosition(w)->value();
        PHLWINDOW  over;
        for (const auto& o : hypr::windows()) {
            if (!o || o == w || !o->m_isX11 || o->isX11OverrideRedirect() || !o->m_isMapped || o->isHidden())
                continue;
            if (const auto os = o->m_xwaylandSurface.lock(); xs->m_pid > 0 && os && os->m_pid > 0 && os->m_pid != xs->m_pid)
                continue; // another app's
            if (o == focus)
                return o;
            if (!over && CBox{hypr::realPosition(o)->value(), hypr::realSize(o)->value()}.containsPoint(at))
                over = o;
        }
        return over;
    }

    namespace {

        // windows in `placed` were put somewhere in the world: their workspace
        // fading in and out doesn't apply to them. `lift`: how much of a fullscreen window's hiding (Hyprland fades the
        // rest of its workspace to 0) is undone for this one, 0..1
        void addWindow(std::vector<SPanel>& out, PHLMONITOR mon, PHLWINDOW w, float depth, int& order, const std::unordered_set<uintptr_t>& placed,
                       float lift = 0.f) {
            const auto ws = w->m_workspace;
            const auto surf = w->wlSurface() ? w->wlSurface()->resource() : nullptr;
            if (!drawable(surf))
                return;

            Vector2D pos = hypr::realPosition(w)->value() + w->m_floatingOffset;
            if (ws && !w->m_pinned)
                pos += ws->m_renderOffset->value();
            pos -= mon->m_position;

            float fs = w->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN);
            if (!hypr::fadingOut(w))
                fs += (1.f - fs) * lift;
            float alpha = w->alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * fs * w->alphaValue(Desktop::View::WINDOW_ALPHA_LAYOUT) *
                w->alphaValue(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE) * w->alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE);
            if (ws && !w->m_pinned && !placed.contains(reinterpret_cast<uintptr_t>(w.get())))
                alpha *= ws->m_alpha->value();
            if (alpha <= 0.f)
                return;

            SPanel p;
            p.key      = reinterpret_cast<uintptr_t>(w.get());
            p.kind     = PANEL_WINDOW;
            p.window   = w;
            p.box      = CBox{pos, hypr::realSize(w)->value()};
            p.depth    = depth;
            p.order    = order++;
            p.alpha    = std::clamp(alpha, 0.f, 1.f);
            p.focused  = w == Desktop::focusState()->window();
            p.rounding = hypr::fullscreenOrMaximized(w) ? 0.f : w->rounding();
            if (p.box.w < 1 || p.box.h < 1)
                return;

            p.hitRoot = surf;
            if (w->m_isX11)
                p.hitScale = surf->m_current.size / p.box.size();
            addTree(p, surf, CBox{{0, 0}, p.box.size()}, !w->m_isX11);

            Vector2D popupBase = p.box.pos();
            if (!w->m_isX11 && w->m_xdgSurface)
                popupBase -= w->m_xdgSurface->m_current.geometry.pos();

            if (!p.surfaces.empty())
                out.emplace_back(std::move(p));

            addPopups(out, w->m_popupHead, popupBase, depth, order, w, alpha);
        }
    }

    std::vector<SPanel> collectPanels(PHLMONITOR mon, float spacing, const std::unordered_set<uintptr_t>& always, float inWorld) {
        std::vector<SPanel> out;
        if (!mon)
            return out;

        int        order = 0;
        const auto ws    = mon->m_activeWorkspace;
        const bool fullscreen = ws && hypr::hasFullscreen(ws);
        // in 3D a fullscreen (or maximized) window hides the rest of its workspace only on the desktop wall, while it's
        // there itself: one out in the world (placed, in tiling mode's row) covers nothing on the wall, and nothing out
        // in the world is under it
        const auto  fsWindow = ws ? hypr::fullscreenWindow(ws) : nullptr;
        const bool  fsOut    = fsWindow && always.contains(reinterpret_cast<uintptr_t>(fsWindow.get()));
        const float in3D     = std::clamp(inWorld, 0.f, 1.f);
        const auto  lift     = [&](const PHLWINDOW& w) { return fsOut || (w && always.contains(reinterpret_cast<uintptr_t>(w.get()))) ? in3D : 0.f; };
        // (Hyprland doesn't render one hidden under a fullscreen window at all)
        const auto  underFs = [&](const PHLWINDOW& w) {
            return fsWindow && w->m_workspace == ws && !w->isAllowedOverFullscreen() && !hypr::fadingOut(w) && w->visibleOnMonitor(mon);
        };

        addLayers(out, mon, 0, DEPTH_BACKGROUND, order);
        addLayers(out, mon, 1, DEPTH_BOTTOM, order);

        std::vector<PHLWINDOW> tiled, floating, full, special, specialFloating, pinned, elsewhere;
        std::vector<std::pair<PHLWINDOW, PHLWINDOW>> x11Popups; // (an X11 menu or tooltip, the window it belongs to)
        PHLWINDOW              focusedTiled;
        for (const auto& w : hypr::windows()) {
            if (!w || w->isHidden() || (!w->m_isMapped && !hypr::fadingOut(w)))
                continue;
            // (an X11 menu or tooltip is under a fullscreen window or not as the window it belongs to is)
            const auto owner = w->m_isX11 && w->isX11OverrideRedirect() ? x11Owner(w) : nullptr;
            if (!g_pHyprRenderer->shouldRenderWindow(w, mon) && !(underFs(w) && lift(owner ? owner : w) > 0.f)) {
                if (w->m_isMapped && always.contains(reinterpret_cast<uintptr_t>(w.get())))
                    elsewhere.push_back(w);
                continue;
            }

            if (owner) {
                x11Popups.emplace_back(w, owner);
                continue;
            }

            if (w->onSpecialWorkspace())
                (w->m_isFloating ? specialFloating : special).push_back(w);
            else if (hypr::fullscreenOrMaximized(w))
                full.push_back(w);
            else if (w->m_pinned)
                pinned.push_back(w);
            else if (w->m_isFloating)
                floating.push_back(w);
            else if (w == Desktop::focusState()->window())
                focusedTiled = w;
            else
                tiled.push_back(w);
        }
        if (focusedTiled)
            tiled.push_back(focusedTiled);

        for (auto& w : tiled)
            addWindow(out, mon, w, DEPTH_TILED, order, always, lift(w));
        for (auto& w : floating) // (with a fullscreen one, those under it that it doesn't hide; the ones over it after it)
            if (!fullscreen || (lift(w) > 0.f && !w->shouldRenderOverFullscreen()))
                addWindow(out, mon, w, DEPTH_FLOATING, order, always, lift(w));
        for (auto& w : full)
            addWindow(out, mon, w, DEPTH_TILED, order, always);
        if (fullscreen) {
            // floating windows allowed over a fullscreen one
            for (auto& w : floating)
                if (w->shouldRenderOverFullscreen())
                    addWindow(out, mon, w, DEPTH_FLOATING, order, always, lift(w));
        }
        for (auto& w : pinned)
            addWindow(out, mon, w, DEPTH_FLOATING, order, always);
        for (auto& w : special)
            addWindow(out, mon, w, DEPTH_SPECIAL, order, always);
        for (auto& w : specialFloating)
            addWindow(out, mon, w, DEPTH_SPECIAL + 1.f, order, always);
        for (auto& w : elsewhere)
            addWindow(out, mon, w, DEPTH_FLOATING, order, always, lift(w));
        // X11 menus and tooltips over the window they belong to, as its popups: carried along when it's placed in the
        // world (their positions are the X server's, relative to where it is on the desktop)
        for (const auto& [w, owner] : x11Popups) {
            const auto   it    = std::ranges::find_if(out, [&](const SPanel& p) { return p.kind == PANEL_WINDOW && p.window.lock() == owner; });
            const bool   owned = it != out.end();
            const float  depth = owned ? it->depth + DEPTH_POPUP : DEPTH_FLOATING;
            const size_t n     = out.size();
            addWindow(out, mon, w, depth, order, always, lift(owner));
            if (owned && out.size() > n) {
                out[n].kind     = PANEL_POPUP;
                out[n].window   = owner;
                out[n].rounding = 0;
            }
        }

        // an input method's popup (its candidates), over the window typed into, which has the keyboard
        if (const auto focus = Desktop::focusState()->window()) {
            const auto   it    = std::ranges::find_if(out, [&](const SPanel& p) { return p.kind == PANEL_WINDOW && p.window.lock() == focus; });
            const float  depth = it != out.end() ? it->depth + DEPTH_POPUP : DEPTH_OVERLAY;
            PROTO::compositor->forEachSurface([&](SP<CWLSurfaceResource> surf) {
                if (!surf || !surf->m_mapped || !drawable(surf))
                    return;
                CInputPopup* ime = g_pInputManager->m_relay.popupFromSurface(surf);
                if (!ime)
                    return;
                SPanel p;
                p.key     = reinterpret_cast<uintptr_t>(ime);
                p.kind    = PANEL_POPUP;
                p.window  = focus;
                p.box     = ime->globalBox().translate(-mon->m_position);
                p.depth   = depth;
                p.order   = order++;
                p.hitRoot = surf;
                addTree(p, surf, CBox{{0, 0}, p.box.size()}, false);
                if (!p.surfaces.empty() && p.box.w >= 1 && p.box.h >= 1)
                    out.emplace_back(std::move(p));
            });
        }

        // (with a fullscreen window, as Hyprland draws them: the ones mapped over it (a notification) at their own alpha,
        // the ones faded out under it not; a bar on the wall, the fullscreen window out in the world, lifted)
        if (!fullscreen)
            addLayers(out, mon, 2, DEPTH_TOP, order);
        else
            addLayers(out, mon, 2, DEPTH_TOP, order, fsOut ? in3D : 0.f);
        addLayers(out, mon, 3, DEPTH_OVERLAY, order);

        // painter's order: deeper (closer to the wall) first, then 2D stacking
        std::ranges::stable_sort(out, [](const SPanel& a, const SPanel& b) {
            if (a.depth != b.depth)
                return a.depth < b.depth;
            return a.order < b.order;
        });

        // levels -> meters; panels sharing a level get pulled apart a little
        // so the ones on top never z-fight with the ones below
        float level = -1.f;
        int   rank  = 0;
        for (auto& p : out) {
            rank    = p.depth == level ? rank + 1 : 0;
            level   = p.depth;
            p.depth = p.depth * spacing + std::min(rank, 30) * RANK_STEP;
        }

        return out;
    }
}
