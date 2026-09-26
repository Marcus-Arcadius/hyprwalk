Title: master: avoid crashing on expired weakptr

Patch: `upstream/hyprland-main-master-guard.patch` in the scratchpad, against main e368c13c: 1 file, +14 −7.
(The cleanup reorder in the follow-up section is `upstream/hyprland-main-root.patch`.)

---

Hyprland segfaults when it quits (`hl.dsp.exit()`, or SIGTERM: a logout, `systemctl stop`) with two or more tiled
windows open in the master layout. #15415 (338bdbb3) fixed the same crash in dwindle; master has the same problem and
still crashes on main (e368c13c). This PR adds the #15415 guard to `CMasterAlgorithm::calculateWorkspace()`: a node
whose `pTarget` has expired is skipped instead of calling `setPositionGlobal()` / `warpPositionSize()` through it.
That covers the single master, the two master-column loops, the three stack loops, and the `warpPositionSize()` loop
in the scope guard.

The reports are in #15097 (and #15096). 0.55.x has the same crash in dwindle too.

### Why a node's target expires

At exit, `CCompositor::cleanup()` clears the window state (`Desktop::windowState()->clear()`, Compositor.cpp:593)
before `wl_display_destroy_clients()` (:604). After that, a window's only strong reference is the render pass from the
last frame: every `CSurfacePassElement` holds a `PHLWINDOW` (SurfacePassElement.hpp:40), and the pass is cleared only
by the next `beginRender()` (Renderer.cpp:1751).

When the first client is destroyed, its window unmaps, and `unmapWindow()` makes a snapshot of it
(`makeSnapshotFB()`, Window.cpp:1624). The snapshot's `beginRender()` clears the render pass, which frees every other
window right there, without `unmapWindow()`. `~CWindow()` doesn't take its layout target out of the layout, so those
windows' master nodes keep expired `pTarget`s. `unmapWindow()` then calls `g_layoutManager->removeTarget()`
(Window.cpp:1659), `CMasterAlgorithm::removeTarget()` calls `calculateWorkspace()`, and that calls
`setPositionGlobal()` with a null `this`:

    segfault at 0 ... in .Hyprland-wrapped
    -> Layout::ITarget::setPositionGlobal(Hyprutils::Math::CBox const&, unsigned char) + 0xd

The whole stack on main, from gdb attached in the VM (master layout, two terminals, `hl.dsp.exit()`):

    Thread 1 ".Hyprland-wrapp" received signal SIGSEGV, Segmentation fault.
    #0  Layout::ITarget::setPositionGlobal(Hyprutils::Math::CBox const&, unsigned char)
    #1  Layout::Tiled::CMasterAlgorithm::calculateWorkspace()
    #2  Layout::Tiled::CMasterAlgorithm::removeTarget(SP<Layout::ITarget>)
    #3  Layout::CAlgorithm::removeTarget(SP<Layout::ITarget>)
    #4  Layout::CSpace::remove(SP<Layout::ITarget>)
    #5  Layout::ITarget::assignToSpace(SP<Layout::CSpace> const&, std::optional<Vector2D>)
    #6  Layout::CLayoutManager::removeTarget(SP<Layout::ITarget>)
    #7  Desktop::View::CWindow::unmapWindow()
        ... CXDGSurfaceResource (unmap) <- CWLSurfaceResource::destroy() <- CWlSurface::onDestroyCalled()
    #20 wl_client_destroy
    #21 wl_display_destroy_clients
    #22 CCompositor::cleanup()
    #23 main

A gdb trace of `~CWindow()` during an exit (0.55.2, the same code path) shows the second window being freed inside
the first window's unmap:

    CWindow::~CWindow()
    CSharedPointer<CWindow>::_delete
    CUniquePointer<CSurfacePassElement> ... deleter
    Render::CRenderPass::clear()
    Render::IHyprRenderer::beginRender(...)
    Render::IHyprRenderer::beginFullFakeRender(...)
    Render::IHyprRenderer::makeSnapshot(SP<CWindow>)
    Desktop::View::CWindow::unmapWindow()
    ... CXDGSurfaceResource destroyed <- CWLSurfaceResource::destroy <- wl_client_destroy
    wl_display_destroy_clients
    CCompositor::cleanup()

A window that the last frame didn't draw (e.g. monocle's background window) is freed by the window-state clear
itself, and leaves a dead node the same way.

### Testing

I ran NixOS VM tests (QEMU/KVM, virtio-gpu, Mesa llvmpipe, 1280x800, `animations.enabled = false`, `foot` terminals
as the windows), using a Lua config and the result of `nix build` from the flake at e368c13c:

| case | main e368c13c | main + this PR |
|---|---|---|
| dwindle, 2 windows, `hl.dsp.exit()` | clean | clean |
| dwindle, 2 windows, SIGTERM | not run | clean |
| master, 2 windows, `hl.dsp.exit()` | **segfault** | clean |
| master, 2 windows, SIGTERM | **segfault** | clean |
| master, 3 windows, `hl.dsp.exit()` | **segfault** | clean |
| master, 3 windows, SIGTERM | not run | clean |
| master, orientation right, 2 windows | **segfault** | clean |
| master, orientation top, 3 windows | not run | clean |
| master, orientation center, 3 windows | not run | clean |
| scrolling, monocle, 2 windows | not run | clean |

The same change backported to 0.55.2, together with 338bdbb3's dwindle hunk, is clean in 19 of 19 exits. Stock
0.55.2 crashes in every dwindle and master case.

### Possible follow-up: the order in `cleanup()`

The guard makes layouts tolerate dead nodes. The underlying cause is the order in `cleanup()`: windows are freed
before their clients are gone, and ~CWindow() doesn't touch the layout. Destroying the clients first, while windows,
workspaces and monitors still exist (plugins are already unloaded at that point), makes every window leave its layout
through the normal unmap path. That's the same path a client disconnect takes at runtime:

```diff
     g_pPluginSystem->unloadAllPlugins();
 
+    g_pXWayland.reset();
+
+    wl_display_destroy_clients(g_pCompositor->m_wlDisplay);
+
     State::Workspace::state()->clear();
     Desktop::windowState()->clear();
     ...
     for (auto const& m : State::monitorState()->monitors()) {
         g_pHyprOpenGL->destroyMonitorResources(m);
     }
 
-    g_pXWayland.reset();
-
-    wl_display_destroy_clients(g_pCompositor->m_wlDisplay);
-
     State::monitorState()->finish();
```

I tested this reorder on main, without the guard, with the same cases as the table: 11 of 11 exits were clean,
and every window unmapped during cleanup. With current main, only one window unmaps and the rest are freed early.
On 0.55.2, the equivalent change on its own gives 19 of 19 clean exits, with every window unmapping.
None of my tests covered X11 windows, layer-shell clients or a locked session at exit.

The current order dates from when windows were unique pointers, before bca7804b. db91d949 moved
`wl_display_destroy_clients()` up once already, for a similar reason. I've kept this PR to the guard; I can open the
reorder separately if you want it.
