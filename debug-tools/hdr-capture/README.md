# HDR capture lab helpers

See [the contract and validation record](../../docs/hdr-capture.md). These tools
never activate NixOS or manage system services. Use the private runtime and
headless parent, **never the real desktop's Wayland/IPC socket**. Patches include
10,000-nit white; do not display them on a physical HDR panel.

## Native ten-bit validation

Build `Hyprland`, `color-managed-source`, and `color-managed-capture` with tests,
or use the installed tools from `nix build .#hyprland-with-tests`. Set `tools` to
`$PWD/build-hdr/hyprtester` (local build) or the package's `bin` directory.

```bash
# Optional: HYPRLAND_BINARY=/nix/store/.../bin/Hyprland
lab=$(debug-tools/hdr-capture/start-lab.sh 1024 512 60)
source "$lab/lab.env"
tools="$PWD/build-hdr/hyprtester"
# The explicit private environment is exported by lab.env. Avoid loader paths
# from another development shell; start a private process group for cleanup.
(
    unset LD_LIBRARY_PATH LD_PRELOAD
    nohup setsid "$tools/color-managed-source" > "$lab/source.log" 2>&1 < /dev/null &
    echo "$!" > "$lab/source.pid"
)
# Wait for "Ready" in source.log before capturing.
(
    unset LD_LIBRARY_PATH LD_PRELOAD
    "$tools/color-managed-capture" Main "$lab/native-shm.raw" --no-cursor
    "$tools/color-managed-capture" Main "$lab/native-dma.raw" --dmabuf --no-cursor
)
python3 debug-tools/hdr-capture/check-native.py "$lab/native-shm.raw" \
    --pair "$lab/native-dma.raw" > "$lab/validation.json"
```

The source uses genuine SHM XBGR2101010 with an acknowledged BT.2020/PQ surface
description: neutral/R/G/B ten-bit ramps, HDR luminance patches, and an SDR white/
gray child at `(0,320)`. The checker validates **every RGB pixel**, all 1,024 ramp
codes, full-range PQ/BT.2020 contracts, and SHM/real DMA-BUF readback agreement.
The GPU probe reads into an integer precision-preserving target, never RGBA8.

Options and additional scenarios:

- Source `--cursor`: controlled 16×16 SDR cursor at `(900,450)`; capture with
  embedded cursor (omit `--no-cursor`) and check with `check-native.py --cursor`.
- Source `--lock`: lock only this private compositor; pending captures must fail.
- Probe `--srgb` and `--untagged`: explicit SDR versus legacy SDR regression.
- Probe `--frames 145 --discard-pixels`: short capture-only throughput; retain
  JSON timing. This is not capture-to-encode or sustained streaming performance.
- Checker `--black`: verify a private-window capture after applying a privacy
  rule **only on the child's IPC endpoint**.
- `start-lab.sh WIDTH HEIGHT REFRESH [off|allow|deny|ask]`: permission scenarios.
  Only the capture probe is matched by the permission rule. For an isolated
  deterministic dialog helper, supply `HDR_LAB_DIALOG_BIN` containing an
  executable `hyprland-dialog`. Never disable real-session permissions.

When done:

```bash
debug-tools/hdr-capture/stop-lab.sh "$lab"
```

Cleanup verifies runtime ownership and each recorded PID's environment/process
group. It only stops groups recorded for that lab; evidence remains on disk.

## Stargaze direct backend

The separate `/projects/rust/stargaze-wayland` worktree vendors the same XML.
With the native fixture running and **the lab's explicit environment exported**:

```bash
# Run from that worktree, not the unrelated /projects/rust/stargaze worktree.
nix develop .#cuda -c cargo test -p stargaze-server wayland_native_rgb10_desktop -- --ignored
nix develop .#cuda -c cargo test -p stargaze-server wayland_desktop_ -- --ignored --nocapture
```

These exercise exact SHM ramps, both capture transports, explicit SDR policy,
independent DMA-BUF lifetime after capture shutdown, and NVENC Main10 software
decode. `wayland_unsupported_compositor` is a separate ignored test: point it at
the **private parent** socket without the experimental extension, not the child.

## Parent compatibility and older mpv experiment

The release-pinned Aquamarine overbinds parent globals (notably xdg_wm_base v6
against Sway's v5). `aquamarine-nested.nix` caps three bindings to advertised
versions **only for the lab**. Normal compositor packages are unchanged. The
parent's SDR output is not an HDR contract: capture renders original surfaces
inside the child before output conversion.

`make-reference.py` and `check-frame.py` remain useful for mpv highlight patches.
When creating an FFV1 clip, set frame properties explicitly with
`setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=gbr:range=full`;
encoder flags alone did not tag the initial experiment. mpv's log must confirm
its Wayland description and actual EGL precision. The recorded NVIDIA mpv run
used eight-bit storage, so it cannot certify ten-bit gradients. Prefer the
native fixture above for precision validation.
