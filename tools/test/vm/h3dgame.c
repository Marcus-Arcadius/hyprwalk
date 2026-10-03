// h3dgame: a tiny SDL2 "game" for tools/test/vm. It prints a line for every input event SDL gives it and draws them, so
// frames change with input. Built by vm.nix; checks.py runs it on Wayland and on XWayland (SDL_VIDEODRIVER=x11).
//
//   h3dgame [--relative] [--grab] [--fullscreen] [--size WxH] [--title T] [--no-vsync] [--log FILE]
//
//   --relative  SDL relative mouse mode (Wayland pointer lock + relative pointer; X11 grab + XInput2 raw motion)
//   --grab      confine the mouse to the window
//   --log FILE  append the lines to FILE, not stdout (a desktop entry's stdout goes nowhere)
//
// Keys: R toggles relative mode, F fullscreen, M maximized; Q or Esc twice quits.
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

int main(int argc, char** argv) {
    int         relative = 0, grab = 0, fullscreen = 0, vsync = 1, w = 640, h = 400;
    const char* title    = "h3dgame";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--relative"))
            relative = 1;
        else if (!strcmp(argv[i], "--grab"))
            grab = 1;
        else if (!strcmp(argv[i], "--fullscreen"))
            fullscreen = 1;
        else if (!strcmp(argv[i], "--no-vsync"))
            vsync = 0;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)
            sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "--title") && i + 1 < argc)
            title = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc && !freopen(argv[++i], "a", stdout))
            return 1;
    }
    // controller events only with the keyboard focus, as the test expects (SDL's default)
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        say("error %s", SDL_GetError());
        return 1;
    }
    say("driver %s", SDL_GetCurrentVideoDriver());
    SDL_Window* win = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h,
                                       SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) {
        say("error %s", SDL_GetError());
        return 1;
    }
    SDL_Renderer* r = SDL_CreateRenderer(win, -1, vsync ? SDL_RENDERER_PRESENTVSYNC : 0);
    if (!r)
        r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (grab)
        SDL_SetWindowGrab(win, SDL_TRUE);
    if (relative)
        say("relative %s", SDL_SetRelativeMouseMode(SDL_TRUE) == 0 ? "on" : "failed");

    long   dx = 0, dy = 0; // summed relative motion
    int    mx = -1, my = -1, keys = 0, buttons = 0, frames = 0, escapes = 0, lastW = 0, lastH = 0;
    Uint32 second = SDL_GetTicks();
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT: say("quit"); return 0;
                case SDL_MOUSEMOTION:
                    dx += e.motion.xrel;
                    dy += e.motion.yrel;
                    mx = e.motion.x;
                    my = e.motion.y;
                    say("motion %d %d %d %d", e.motion.x, e.motion.y, e.motion.xrel, e.motion.yrel);
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                    buttons += e.type == SDL_MOUSEBUTTONDOWN;
                    say("button %s %d %d %d", e.type == SDL_MOUSEBUTTONDOWN ? "down" : "up", e.button.button, e.button.x, e.button.y);
                    break;
                case SDL_MOUSEWHEEL: say("wheel %d %d", e.wheel.x, e.wheel.y); break;
                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    if (e.key.repeat)
                        break;
                    say("key %s %s", e.type == SDL_KEYDOWN ? "down" : "up", SDL_GetScancodeName(e.key.keysym.scancode));
                    if (e.type != SDL_KEYDOWN)
                        break;
                    ++keys;
                    if (e.key.keysym.scancode == SDL_SCANCODE_R)
                        say("relative %s", SDL_SetRelativeMouseMode(!SDL_GetRelativeMouseMode()) == 0 ? (SDL_GetRelativeMouseMode() ? "on" : "off") : "failed");
                    else if (e.key.keysym.scancode == SDL_SCANCODE_F) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        say("fullscreen %s", fullscreen ? "on" : "off");
                    } else if (e.key.keysym.scancode == SDL_SCANCODE_M) {
                        const int max = !(SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED);
                        if (max)
                            SDL_MaximizeWindow(win);
                        else
                            SDL_RestoreWindow(win);
                        say("maximize %s", max ? "on" : "off");
                    } else if (e.key.keysym.scancode == SDL_SCANCODE_Q || (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE && ++escapes >= 2)) {
                        say("quit");
                        return 0;
                    }
                    break;
                case SDL_WINDOWEVENT:
                    switch (e.window.event) {
                        case SDL_WINDOWEVENT_FOCUS_GAINED: say("focus gained"); break;
                        case SDL_WINDOWEVENT_FOCUS_LOST: say("focus lost"); break;
                        case SDL_WINDOWEVENT_ENTER: say("mouse enter"); break;
                        case SDL_WINDOWEVENT_LEAVE: say("mouse leave"); break;
                        case SDL_WINDOWEVENT_RESIZED: // sdl2-compat on SDL3 may send only this one
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                            if (e.window.data1 != lastW || e.window.data2 != lastH)
                                say("size %d %d", lastW = e.window.data1, lastH = e.window.data2);
                            break;
                        default: break;
                    }
                    break;
                case SDL_CONTROLLERDEVICEADDED: {
                    SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which);
                    say("controller added %s", c ? SDL_GameControllerName(c) : SDL_GetError());
                    break;
                }
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP:
                    ++buttons;
                    say("cbutton %s %s", e.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up", SDL_GameControllerGetStringForButton(e.cbutton.button));
                    break;
                case SDL_CONTROLLERAXISMOTION:
                    if (e.caxis.value > 16000 || e.caxis.value < -16000)
                        say("caxis %s %d", SDL_GameControllerGetStringForAxis(e.caxis.axis), e.caxis.value);
                    break;
                default: break;
            }
        }
        // draw the input: background from relative motion, top band from keys and buttons, a square at the pointer
        int ow = 0, oh = 0;
        SDL_GetRendererOutputSize(r, &ow, &oh);
        SDL_SetRenderDrawColor(r, (Uint8)(40 + ((dx / 2) % 200 + 200) % 200), (Uint8)(40 + ((dy / 2) % 200 + 200) % 200), 90, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, (Uint8)(keys * 53), (Uint8)(buttons * 97), 255, 255);
        SDL_Rect band = {0, 0, ow, oh / 8};
        SDL_RenderFillRect(r, &band);
        if (mx >= 0) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(win, &ww, &wh);
            const float sx = ww ? (float)ow / ww : 1, sy = wh ? (float)oh / wh : 1;
            SDL_Rect    at  = {(int)(mx * sx) - 6, (int)(my * sy) - 6, 12, 12};
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            SDL_RenderFillRect(r, &at);
        }
        SDL_RenderPresent(r);
        ++frames;
        if (!vsync)
            SDL_Delay(4);
        if (SDL_GetTicks() - second >= 1000) {
            say("frames %d", frames);
            frames = 0;
            second += 1000;
        }
    }
}
