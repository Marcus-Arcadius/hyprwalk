# the user's Hyprland (zaneyos/modules/astroland/packages.nix, with its flake.lock's inputs), and extra patches
{ patches ? [ ] }:
let
  zaneyos = builtins.getFlake "git+file:///home/monero/zaneyos";
  astroland = import "${zaneyos}/modules/astroland/packages.nix" {
    inputs = zaneyos.inputs;
    system = "x86_64-linux";
    username = "monero";
    monitorOutputs = [ "HDMI-A-1" "DP-1" ];
  };
in
if patches == [ ] then astroland.hyprland
else astroland.hyprland.overrideAttrs (old: { patches = (old.patches or [ ]) ++ patches; })
