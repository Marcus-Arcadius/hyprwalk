# aquamarine from zaneyos's hyprland-astroland input, with extra patches
{ patches ? [ ] }:
let
  zaneyos = builtins.getFlake "git+file:///home/monero/zaneyos";
  aq = zaneyos.inputs.hyprland-astroland.inputs.aquamarine.packages.x86_64-linux.aquamarine;
in
if patches == [ ] then aq else aq.overrideAttrs (old: { patches = (old.patches or [ ]) ++ patches; })
