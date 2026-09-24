#include "panels.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/render/Renderer.hpp>

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
                    ps.tex  = s->m_current.texture;
                    ps.uvTL = uvFromViewport(s, ps.uvBR);

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
                    p.alpha  = std::clamp(popup->m_alpha->value() * alphaMul, 0.f, 1.f);
                    p.hitRoot = surf;
                    addTree(p, surf, CBox{{0, 0}, p.box.size()}, false);
                    if (!p.surfaces.empty() && p.alpha > 0.f)
                        out.emplace_back(std::move(p));
                },
                nullptr);
        }

        void addLayers(std::vector<SPanel>& out, PHLMONITOR mon, int layer, float depth, int& order) {
            for (const auto& ref : mon->m_layerSurfaceLayers[layer]) {
                const auto ls = ref.lock();
                if (!ls || (!ls->m_mapped && !ls->m_fadingOut))
                    continue;
                const auto surf = ls->resource();
                if (!drawable(surf))
                    continue;

                const float alpha = ls->m_alpha->value();
                if (alpha <= 0.f)
                    continue;

                SPanel p;
                p.key   = reinterpret_cast<uintptr_t>(ls.get());
                p.kind  = PANEL_LAYER;
                p.layer = layer;
                p.box   = CBox{ls->m_realPosition->value() - mon->m_position, ls->m_realSize->value()};
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

        // windows in `placed` were put somewhere in the world: their workspace
        // fading in and out doesn't apply to them
        void addWindow(std::vector<SPanel>& out, PHLMONITOR mon, PHLWINDOW w, float depth, int& order, const std::unordered_set<uintptr_t>& placed) {
            const auto ws = w->m_workspace;
            const auto surf = w->wlSurface() ? w->wlSurface()->resource() : nullptr;
            if (!drawable(surf))
                return;

            Vector2D pos = w->m_realPosition->value() + w->m_floatingOffset;
            if (ws && !w->m_pinned)
                pos += ws->m_renderOffset->value();
            pos -= mon->m_position;

            float alpha = w->alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * w->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN) *
                w->alphaValue(Desktop::View::WINDOW_ALPHA_LAYOUT) * w->alphaValue(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE) *
                w->alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE);
            if (ws && !w->m_pinned && !placed.contains(reinterpret_cast<uintptr_t>(w.get())))
                alpha *= ws->m_alpha->value();
            if (alpha <= 0.f)
                return;

            SPanel p;
            p.key      = reinterpret_cast<uintptr_t>(w.get());
            p.kind     = PANEL_WINDOW;
            p.window   = w;
            p.box      = CBox{pos, w->m_realSize->value()};
            p.depth    = depth;
            p.order    = order++;
            p.alpha    = std::clamp(alpha, 0.f, 1.f);
            p.focused  = w == Desktop::focusState()->window();
            p.rounding = w->isFullscreen() ? 0.f : w->rounding();
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

    std::vector<SPanel> collectPanels(PHLMONITOR mon, float spacing, const std::unordered_set<uintptr_t>& always) {
        std::vector<SPanel> out;
        if (!mon)
            return out;

        int        order = 0;
        const auto ws    = mon->m_activeWorkspace;
        const bool fullscreen = ws && ws->m_hasFullscreenWindow && ws->m_fullscreenMode == FSMODE_FULLSCREEN;

        addLayers(out, mon, 0, DEPTH_BACKGROUND, order);
        addLayers(out, mon, 1, DEPTH_BOTTOM, order);

        std::vector<PHLWINDOW> tiled, floating, full, special, specialFloating, pinned, elsewhere;
        PHLWINDOW              focusedTiled;
        for (const auto& w : g_pCompositor->m_windows) {
            if (!w || w->isHidden() || (!w->m_isMapped && !w->m_fadingOut))
                continue;
            if (!g_pHyprRenderer->shouldRenderWindow(w, mon)) {
                if (w->m_isMapped && always.contains(reinterpret_cast<uintptr_t>(w.get())))
                    elsewhere.push_back(w);
                continue;
            }

            if (w->onSpecialWorkspace())
                (w->m_isFloating ? specialFloating : special).push_back(w);
            else if (w->isFullscreen())
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
            addWindow(out, mon, w, DEPTH_TILED, order, always);
        for (auto& w : fullscreen ? std::vector<PHLWINDOW>{} : floating)
            addWindow(out, mon, w, DEPTH_FLOATING, order, always);
        for (auto& w : full)
            addWindow(out, mon, w, DEPTH_TILED, order, always);
        if (fullscreen) {
            // floating windows allowed over a fullscreen one
            for (auto& w : floating)
                if (w->shouldRenderOverFullscreen())
                    addWindow(out, mon, w, DEPTH_FLOATING, order, always);
        }
        for (auto& w : pinned)
            addWindow(out, mon, w, DEPTH_FLOATING, order, always);
        for (auto& w : special)
            addWindow(out, mon, w, DEPTH_SPECIAL, order, always);
        for (auto& w : specialFloating)
            addWindow(out, mon, w, DEPTH_SPECIAL + 1.f, order, always);
        for (auto& w : elsewhere)
            addWindow(out, mon, w, DEPTH_FLOATING, order, always);

        if (!fullscreen)
            addLayers(out, mon, 2, DEPTH_TOP, order);
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
