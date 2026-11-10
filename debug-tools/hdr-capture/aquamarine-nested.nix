# Lab-only workaround for the release pin's Wayland backend overbinding globals.
# Does not alter Hyprland's normal packages or the deployed compositor.
# nix build --impure --no-link --print-out-paths --file debug-tools/hdr-capture/aquamarine-nested.nix
let
  flake = builtins.getFlake (toString ../..);
in
flake.inputs.aquamarine.packages.${builtins.currentSystem}.default.overrideAttrs (old: {
  pname = "aquamarine-hdr-nested-lab";
  postPatch = (old.postPatch or "") + ''
    substituteInPlace src/backend/Wayland.cpp \
      --replace-fail '&wl_seat_interface, 9)' '&wl_seat_interface, std::min(version, 9U))' \
      --replace-fail '&xdg_wm_base_interface, 6)' '&xdg_wm_base_interface, std::min(version, 6U))' \
      --replace-fail '&wl_compositor_interface, 6)' '&wl_compositor_interface, std::min(version, 6U))'
  '';
})
