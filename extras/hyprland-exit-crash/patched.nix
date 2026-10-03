# zaneyos's Hyprland (modules/astroland/packages.nix, its flake.lock inputs) with extra patches
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
