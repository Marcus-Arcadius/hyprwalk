# The NixOS VMs that tools/test/vm/run.sh tests hypr3d in, the way Hyprland's own CI tests Hyprland
# (nix/tests/default.nix in its repo): QEMU with KVM and a virtio GPU (Mesa's llvmpipe draws), the very
# Hyprland hypr3d.so was built for, started as alice's login session on tty1 the way a display manager
# would, and PipeWire with a virtual microphone. The driver starts QEMU with -nographic, and so does
# virtualisation.graphics = false, so no window opens anywhere (with gpu = "virgl", QEMU's egl-headless display,
# which draws on a render node and opens no window either).
#
#   nix-build tools/test/vm/vm.nix -A driver --argstr hyprland /nix/store/...-hyprland-...
#
# There are two VMs, the same but for the screen: `machine` at 1280x800, and `hidpi` at 1920x1200 for scales 1.5
# and 2. (virtio-gpu only takes the mode it prefers, the xres and yres it's given: any other one fails DRM's atomic
# test, so a monitor can't change its resolution in a VM.) The checks are tools/test/vm/checks.py, which run.sh hands
# the driver (--test-script), so changing them doesn't rebuild anything.
{
  hyprland, # the Hyprland's store path (the one build.sh built against)
  nixpkgs ? <nixpkgs>,
  cores ? 8, # llvmpipe draws with all of them
  # "llvmpipe": Mesa draws in software on a plain virtio GPU. "virgl": a GPU of the host's draws, through
  # virglrenderer: QEMU's egl-headless display on its render node (it opens no window)
  gpu ? "llvmpipe",
  rendernode ? "/dev/dri/renderD129", # the Intel iGPU here (renderD128 is the NVIDIA that runs the desktop)
}:
let
  pkgs = import nixpkgs { };
  # the one that's installed, with its closure (not in pure evaluation mode: nix-build is fine)
  hypr = builtins.storePath hyprland;

  # h3dgame.c: a tiny SDL2 game that prints what it gets (the mouse, relative motion, keys, controllers, focus)
  h3dgame = pkgs.runCommandCC "h3dgame" {
    nativeBuildInputs = [ pkgs.pkg-config ];
    buildInputs = [ pkgs.SDL2 ];
  } ''
    mkdir -p $out/bin
    $CC -O2 -Wall ${./h3dgame.c} -o $out/bin/h3dgame $(pkg-config --cflags --libs sdl2)
  '';

  # a VM with a screen of that size
  vm =
    width: height:
    { config, pkgs, ... }:
    {
      system.stateVersion = "26.11";

      virtualisation = {
        inherit cores;
        memorySize = 6144; # (a browser and OBS on llvmpipe)
        # (no -nographic with virgl: it would take the place of egl-headless, a display that opens no window either)
        graphics = gpu == "virgl";
        diskSize = 8192; # (a sparse image) room for a core dump, so a crash's stack trace can be had
        # no VGA, a virtio GPU without 3D: Mesa's llvmpipe draws, through GBM on its DRM device (or, with gpu =
        # "virgl", one with 3D). No VMware port either: through it the PS/2 mouse turns into an absolute vmmouse, and
        # there'd be no relative mouse (the USB tablet is the absolute one)
        qemu.options = [
          "-vga none"
          "-machine vmport=off"
        ]
        ++ (
          if gpu == "virgl" then
            [
              "-device virtio-gpu-gl-pci,xres=${toString width},yres=${toString height}"
              "-display egl-headless,rendernode=${rendernode}"
            ]
          else
            [ "-device virtio-gpu-pci,xres=${toString width},yres=${toString height}" ]
        );
      };

      hardware.graphics.enable = true;
      # the Action Menu's text: Latin, Japanese (the test avatars' names) and emoji
      fonts.packages = with pkgs; [
        noto-fonts
        noto-fonts-cjk-sans
        noto-fonts-color-emoji
      ];
      services.speechd.enable = false; # (Hyprland's CI turns it off too)

      users.users.alice = {
        isNormalUser = true;
        uid = 1000;
        password = "h3d"; # (for swaylock, which play mode must give the keyboard to)
        extraGroups = [
          "video"
          "audio"
          "input"
        ];
      };

      environment.systemPackages = [
        hypr
        h3dgame
      ]
      ++ (with pkgs; [
        foot
        gdb
        grim
        jq
        pipewire
        # wheel.py, touchpad.py and gamepad.py (a mouse, a touchpad and a game controller, through uinput), and
        # tkapp.py (an X11 app with menus and a tooltip)
        (python3.withPackages (ps: [ ps.tkinter ]))
        wev # prints the pointer and keyboard events its window gets
        wireplumber
        xwayland # (Hyprland starts it when it finds it)
        xev # wev for X11 windows
        xterm
        swayidle # the screen blanking, to see that games and videos keep it from it
        weston # its demo clients: weston-presentation-shm (presentation feedback), weston-dnd
        # real open-source games: Chocolate Doom (mouse look, the pointer locked) with Freedoom's levels, and
        # SuperTux (the keyboard, or a controller)
        chocolate-doom
        freedoom
        supertux
        # everyday apps, open-source stand-ins: two browsers, Electron (Discord's stack), OBS, and what they need:
        # a notification daemon, the clipboard's tools
        chromium
        firefox
        electron
        obs-studio
        mako
        wl-clipboard
        fcitx5
        dbus # (dbus-monitor)
        swaylock
      ]);
      security.pam.services.swaylock = { };
      # The portals' user services want graphical-session.target, which a desktop's session brings up: Hyprland
      # started this way doesn't, so the checks start this (home-manager's hyprland-session.target is the same)
      systemd.user.targets.hyprland-session = {
        description = "Hyprland session (tools/test/vm)";
        bindsTo = [ "graphical-session.target" ];
        wants = [ "graphical-session-pre.target" ];
        after = [ "graphical-session-pre.target" ];
      };
      # (what the portal decides, in the journal; xdph's log line by line: to the journal's pipe it'd come in blocks)
      systemd.user.services.xdg-desktop-portal.environment.G_MESSAGES_DEBUG = "all";
      systemd.user.services.xdg-desktop-portal-hyprland.environment = {
        LD_PRELOAD = "${pkgs.coreutils}/libexec/coreutils/libstdbuf.so";
        _STDBUF_O = "L";
      };
      # Freedoom's levels, for Chocolate Doom
      environment.etc."h3d/freedoom2.wad".source = "${pkgs.freedoom}/share/games/doom/freedoom2.wad";
      # file dialogs and screen capture through the portals, as a Hyprland desktop has them
      xdg.portal = {
        enable = true;
        extraPortals = with pkgs; [
          xdg-desktop-portal-hyprland
          xdg-desktop-portal-gtk
        ];
        config.common.default = [
          "hyprland"
          "gtk"
        ];
      };

      # Hyprland on tty1 in a logind session of alice's, as a display manager starts it (the NixOS cage
      # module's way); the checks start and stop it
      systemd.services.hyprland = {
        description = "Hyprland on tty1 for tools/test/vm";
        after = [
          "systemd-user-sessions.service"
          "systemd-logind.service"
          "getty@tty1.service"
        ];
        wants = [
          "dbus.socket"
          "systemd-logind.service"
        ];
        conflicts = [ "getty@tty1.service" ];
        restartIfChanged = false;
        path = [ config.system.path ];
        environment = {
          XDG_SESSION_TYPE = "wayland";
          XDG_CURRENT_DESKTOP = "Hyprland";
        };
        serviceConfig = {
          # the checks write the config, and which one it is (a .lua or a .conf) to /run/hyprland-test.env
          EnvironmentFile = "/run/hyprland-test.env";
          ExecStart = "${hypr}/bin/start-hyprland --path ${hypr}/bin/Hyprland -- --config \${H3D_CONFIG}";
          User = "alice";
          WorkingDirectory = "/home/alice";
          IgnoreSIGPIPE = "no";
          UtmpIdentifier = "%n";
          UtmpMode = "user";
          TTYPath = "/dev/tty1";
          TTYReset = "yes";
          TTYVHangup = "yes";
          TTYVTDisallocate = "yes";
          StandardInput = "tty-fail";
          StandardOutput = "journal";
          StandardError = "journal";
          PAMName = "hyprland-test";
          # without it, Hyprland (and all it starts) had CAP_WAKE_ALARM here, which the portals, without it,
          # may not look into (/proc/PID/root: EACCES), so they refused every app Hyprland started (OBS's screen
          # capture). A desktop's Hyprland has at most CAP_SYS_NICE, which its apps don't inherit
          AmbientCapabilities = "";
          CapabilityBoundingSet = "~CAP_WAKE_ALARM";
        };
      };
      security.pam.services.hyprland-test.text = ''
        auth     required ${pkgs.pam}/lib/security/pam_unix.so nullok
        account  required ${pkgs.pam}/lib/security/pam_unix.so
        session  required ${pkgs.pam}/lib/security/pam_unix.so
        session  required ${pkgs.pam}/lib/security/pam_env.so conffile=/etc/pam/environment readenv=0
        session  required ${config.systemd.package}/lib/security/pam_systemd.so
      '';

      # PipeWire, and a microphone to sing into: what's played into the "Test microphone in" sink comes out of
      # the "Test microphone" source, the only source there is
      security.rtkit.enable = true;
      services.pipewire = {
        enable = true;
        alsa.enable = false;
        pulse.enable = false;
        wireplumber.enable = true;
        extraConfig.pipewire."90-test-microphone" = {
          "context.modules" = [
            {
              name = "libpipewire-module-loopback";
              args = {
                "node.description" = "Test microphone";
                "audio.position" = [ "MONO" ];
                "capture.props" = {
                  "node.name" = "test_mic_in";
                  "node.description" = "Test microphone in";
                  "media.class" = "Audio/Sink";
                };
                "playback.props" = {
                  "node.name" = "test_mic";
                  "node.description" = "Test microphone";
                  "media.class" = "Audio/Source";
                };
              };
            }
          ];
        };
      };
    };
in
pkgs.testers.runNixOSTest {
  name = "hypr3d-vm";
  testScript = "raise Exception('run tools/test/vm/run.sh: it gives the driver the test script')";
  skipLint = true;
  skipTypeCheck = true;
  # (the tests' own QEMU has no OpenGL)
  qemu.package = if gpu == "virgl" then pkgs.qemu else pkgs.qemu_test;

  nodes.machine = vm 1280 800;
  nodes.hidpi = vm 1920 1200;
}
