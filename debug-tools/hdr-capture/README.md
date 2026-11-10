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
- Source `--sdr`: an independent, untagged sRGB white/gray toplevel. Float it at
  `(64,64)` with size 512×128 over a paused HDR application and validate with
  `check-overlay.py BACKGROUND.raw MIXED.raw`. Both captures must omit cursors.
  Keep Main at global `(0,0)`; monitor auto-placement can change window positions.
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

## Sustained capture-to-encode and image comparison

The native fixture must cover the entire output at scale 1. After launching a
3440×1440/145 private lab and exporting its `lab.env`, run from the Stargaze
worktree (with no other live tests in parallel):

```bash
STARGAZE_TEST_SECONDS=120 STARGAZE_TEST_FPS=145 \
  STARGAZE_TEST_REPORT="$XDG_RUNTIME_DIR/native-dma.json" \
  nix develop .#cuda -c cargo test -p stargaze-server wayland_capture_encode_endurance -- --ignored --nocapture --test-threads=1
# Add STARGAZE_TEST_SHM=1 for the explicit, slower SHM diagnostic.
# Set STARGAZE_TEST_NATIVE=0 for arbitrary HDR application content instead of ramps.
```

The test reports rate, packet timing, per-second RSS/FD samples, and saves a
final recovery GOP (`.hevc`) plus its last decoded YUV420P10LE frame (`.yuv`).
It requests one late IDR, not periodic IDRs: production intentionally uses an
infinite GOP. The final GOP is retained only briefly to avoid measuring a leak
created by the test itself. The test checks packet continuity and decoded HDR
color/layout, with native ramp assertions when applicable. Software decoding
happens after the measured interval. Timing is host-side and excludes network,
client display, and scene rendering before capture delivery.

`wayland_capture_source_changes` resizes, changes color, then removes Main.
It must run **alone**, with `--ignored --nocapture --test-threads=1`; the lab's
output/source will need recreation afterwards. It validates termination rather
than silently feeding changed-contract frames to a fixed-mode encoder.

For a paused scene, compare a raw capture to that final decoded frame:

```bash
python3 debug-tools/hdr-capture/compare-encode.py --self-test
python3 debug-tools/hdr-capture/compare-encode.py "$lab/frame.raw" "$lab/run.yuv" \
  --reference-yuv "$lab/reference.yuv" > "$lab/comparison.json"
```

This computes full-frame Y/U/V code errors and PSNR against the known BT.2020-NCL
matrix and 2×2 chroma average. Capture again after the run and require byte-for-
byte agreement with the first raw frame: seeking a paused player can return
before decoding/rendering settles. A mismatched scene must not be accepted as
an encoder comparison.

For photographic application testing, mpv Vulkan works on the recorded driver:
`--vo=gpu-next --gpu-api=vulkan --gpu-context=waylandvk --target-colorspace-hint=yes
--target-prim=bt.2020 --target-trc=pq --target-peak=10000 --tone-mapping=clip
--dither=no --no-audio --osc=no --osd-level=0`. Confirm the log's actual swapchain
format (`R16G16B16A16_SFLOAT` in this run) AND ready Wayland PQ description; an
initial RGB10/sRGB configuration before the first decoded HDR frame is not the
final contract. Use `--pause=yes` for image comparisons. The downloaded photo
demo and resulting images are not distributed in this repository; provenance
and hashes are in [the numerical record](validation/2026-11-10.json).

To inspect comparable **SDR previews** of the reference and decoded YUV frames,
apply the same explicit input metadata and tone mapper to each, for example:

```bash
ffmpeg -f rawvideo -pixel_format yuv420p10le -video_size 1920x1080 -i frame.yuv \
  -vf 'zscale=pin=bt2020:tin=smpte2084:min=bt2020nc:rin=limited:p=bt2020:t=linear:m=gbr:r=full:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=mobius:desat=0,zscale=t=iec61966-2-1:m=gbr:r=full,format=rgb24,scale=960:540' \
  -frames:v 1 preview.png
```

Preview similarity is not an HDR panel measurement. See the validation record
for measured native/application rates and remaining limits.

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
