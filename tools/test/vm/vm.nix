# NixOS VMs for tools/test/vm/run.sh, set up as Hyprland's own CI tests Hyprland (nix/tests/default.nix): QEMU with
# KVM and a virtio GPU, the very Hyprland hypr3d.so was built for as alice's tty1 login session, and PipeWire with a
# virtual microphone. No window opens (-nographic, or egl-headless with gpu = "virgl").
#
#   nix-build tools/test/vm/vm.nix -A driver --argstr hyprland /nix/store/...-hyprland-...
#
# `machine` is 1280x800, `hidpi` 1920x1200 for scales 1.5 and 2: virtio-gpu only takes the xres and yres it's given
# (other modes fail DRM's atomic test). run.sh hands the driver checks.py, so changing the checks rebuilds nothing.
{
  hyprland, # store path of the Hyprland build.sh built against
  nixpkgs ? <nixpkgs>,
  cores ? 8, # llvmpipe draws with all of them
  # "llvmpipe": Mesa's software rendering on a plain virtio GPU; "virgl": a host GPU through virglrenderer (QEMU's
  # egl-headless display on its render node)
  gpu ? "llvmpipe",
  rendernode ? "/dev/dri/renderD129", # the iGPU here; renderD128 drives the desktop
}:
let
  pkgs = import nixpkgs { };
  # the installed one with its closure (impure: fine with nix-build, not in pure evaluation)
  hypr = builtins.storePath hyprland;

  # h3dgame.c: a tiny SDL2 game that prints the input it gets
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
        memorySize = 6144; # a browser and OBS on llvmpipe
        # virgl needs a display: -nographic would replace egl-headless, which opens no window either
        graphics = gpu == "virgl";
        diskSize = 8192; # sparse; room for a core dump's stack trace
        # no VGA; a virtio GPU without 3D (llvmpipe draws through GBM), or with 3D for virgl. No VMware port: it turns
        # the PS/2 mouse into an absolute vmmouse, leaving no relative mouse
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
      services.speechd.enable = false; # Hyprland's CI turns it off too

      users.users.alice = {
        isNormalUser = true;
        uid = 1000;
        password = "h3d"; # for swaylock (play mode must yield the keyboard)
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
        # for wheel.py, touchpad.py and gamepad.py (a uinput mouse, touchpad and game controller) and tkapp.py (an X11
        # app with menus and a tooltip)
        (python3.withPackages (ps: [ ps.tkinter ]))
        wev # prints its window's pointer and key events
        wireplumber
        xwayland # Hyprland starts it if found
        xev # wev for X11 windows
        xterm
        swayidle # screen blanking, which games and videos must inhibit
        weston # weston-presentation-shm, weston-dnd demo clients
        # real open-source games: Chocolate Doom with Freedoom's levels (mouse look, pointer locked) and SuperTux
        # (keyboard or controller)
        chocolate-doom
        freedoom
        supertux
        # open-source stand-ins for everyday apps: two browsers, Electron (Discord's stack), OBS, a notification daemon,
        # clipboard tools
        chromium
        firefox
        electron
        obs-studio
        mako
        wl-clipboard
        fcitx5
        dbus # dbus-monitor
        swaylock
        swaybg # a layer-surface wallpaper for the crosshair to start on
        quickshell # full-screen see-through overlay (overlay.qml)
      ]);
      security.pam.services.swaylock = { };
      # the portals' user services want graphical-session.target, which Hyprland started this way doesn't bring up: the
      # checks start this target (as home-manager's hyprland-session.target)
      systemd.user.targets.hyprland-session = {
        description = "Hyprland session (tools/test/vm)";
        bindsTo = [ "graphical-session.target" ];
        wants = [ "graphical-session-pre.target" ];
        after = [ "graphical-session-pre.target" ];
      };
      # portal decisions in the journal; xdph's log line-buffered (into the journal's pipe it would come in blocks)
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

      # Hyprland on tty1 in alice's logind session, as a display manager starts it (like the NixOS cage module); the
      # checks start and stop it
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
          # the checks write the config and put its path (.lua or .conf) in /run/hyprland-test.env
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
          # drop CAP_WAKE_ALARM: Hyprland's children would inherit it, and the portals, lacking it, can't read their
          # /proc/PID/root (EACCES) and refuse them (OBS's screen capture). A desktop's Hyprland has at most
          # CAP_SYS_NICE, which its apps don't inherit
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

      # PipeWire with a microphone to sing into: what's played into the "Test microphone in" sink comes out of the "Test
      # microphone" source, the only source
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
  # a full llvmpipe run takes about an hour; the driver's default 3600 s would cut off its last sections
  globalTimeout = 7200;
  # qemu_test has no OpenGL
  qemu.package = if gpu == "virgl" then pkgs.qemu else pkgs.qemu_test;

  nodes.machine = vm 1280 800;
  nodes.hidpi = vm 1920 1200;
}
