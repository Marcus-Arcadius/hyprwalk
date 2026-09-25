# The NixOS VM that tools/test/vm/run.sh tests hypr3d in, the way Hyprland's own CI tests Hyprland
# (nix/tests/default.nix in its repo): QEMU with KVM and a virtio GPU (Mesa's llvmpipe draws), the very
# Hyprland hypr3d.so was built for, started as alice's login session on tty1 the way a display manager
# would, and PipeWire with a virtual microphone. The driver starts QEMU with -nographic, and so does
# virtualisation.graphics = false, so no window opens anywhere.
#
#   nix-build tools/test/vm/vm.nix -A driver --argstr hyprland /nix/store/...-hyprland-...
#
# The checks are tools/test/vm/checks.py, which run.sh hands the driver (--test-script), so changing them
# doesn't rebuild anything.
{
  hyprland, # the Hyprland's store path (the one build.sh built against)
  nixpkgs ? <nixpkgs>,
  cores ? 8, # llvmpipe draws with all of them
}:
let
  pkgs = import nixpkgs { };
  # the one that's installed, with its closure (not in pure evaluation mode: nix-build is fine)
  hypr = builtins.storePath hyprland;
in
pkgs.testers.runNixOSTest {
  name = "hypr3d-vm";
  testScript = "raise Exception('run tools/test/vm/run.sh: it gives the driver the test script')";
  skipLint = true;
  skipTypeCheck = true;

  nodes.machine =
    { config, pkgs, ... }:
    {
      system.stateVersion = "26.11";

      virtualisation = {
        inherit cores;
        memorySize = 4096;
        graphics = false;
        diskSize = 8192; # (a sparse image) room for a core dump, so a crash's stack trace can be had
        # no VGA, a virtio GPU without 3D: Mesa's llvmpipe draws, through GBM on its DRM device. No VMware port
        # either: through it the PS/2 mouse turns into an absolute vmmouse, and there'd be no relative mouse (the
        # USB tablet is the absolute one)
        qemu.options = [
          "-vga none"
          "-device virtio-gpu-pci,xres=1280,yres=800"
          "-machine vmport=off"
        ];
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
        extraGroups = [
          "video"
          "audio"
          "input"
        ];
      };

      environment.systemPackages = [
        hypr
      ]
      ++ (with pkgs; [
        foot
        gdb
        grim
        jq
        pipewire
        wireplumber
      ]);

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
}
