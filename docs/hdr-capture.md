# Experimental color-managed capture (personal fork)

## Release policy and status

This private patch series starts at **v0.57.2**, commit
`11cbc6c577b84ffb1edf4df3fc5b6128c3ae6947`. For each official release, preserve
this branch, create a new release-based branch, replay the patches, and repeat
the checks below. Do not silently follow upstream `main` or activate a new
compositor while testing. **No upstream submission is planned.**

**Isolated reference-fixture validation passes; not deployed.** Native RGB10
Wayland surfaces retain all 1,024 neutral and primary-channel values. Real
DMA-BUF readback matches SHM exactly. Stargaze captures both backends and encodes
actual desktop frames as NVENC Main10/PQ. Real application/game content,
end-to-end sustained target-rate streaming, and client display validation remain
separate gates. An HDR output or ten-bit buffer alone does not establish HDR
capture.

## Protocol and opt-in

Enable only in an isolated compositor configuration, then restart that instance:

```lua
hl.config({
    experimental = { xx_image_capture_color_management = true },
})
```

This new private-fork option defaults off and requires `render.cm_enabled` and
`experimental.wp_cm_1_2` (both normally enabled). Existing options keep their
meaning. Configuration documentation stays in this fork; there is no upstream
wiki PR or automatic upstream interaction.

The vendored XML is **unmodified** from wayland-protocols commit
`aa62366fb800a0689f4e9de83811ff33f6a91f44`, path
`experimental/xx-image-capture-color-management/xx-image-capture-color-management-v1.xml`.
Capture clients must pin the identical experimental XML.

Clients:

1. Create an ordinary output/workspace image-copy session.
2. Query `get_capture_source_colors`. Its preferred description is only a hint,
   not a frame contract.
3. Resolve the frozen reference from `get_preferred` using color-manager v2+
   `get_image_description`, then inspect `get_information`.
4. Create a ready parametric BT.2020/PQ description and apply it to **each
   frame**, before `capture`, using `set_frame_image_description`.

A failed/not-ready description or a change after requesting capture is a
protocol error. Descriptions are frozen for each requested frame, including
when their protocol objects are subsequently destroyed. Source-color objects
survive destruction of the source protocol object and become inert when the
backing source disappears. References are immutable.

## Rendering and compatibility

Explicit descriptions select a separate scene render, not the sRGB mirror:

- A session-owned **BT.2020 linear FP16** scene retains absolute 0–10,000-nit
  luminance before export conversion. Unscaled linear luminance avoids FP16
  subnormal precision loss near black.
- The immutable requested target controls both DMA-BUF output and SHM readback.
- PQ/HLG/extended-linear exports require XBGR2101010 or ABGR2101010. Missing FP16,
  unsupported sources/ICC targets, or incompatible buffers fail, rather than
  producing an SDR frame labelled HDR.
- Monitor/workspace sources are supported; region, window, and mirrored-output
  sources are not. Separately captured cursors support only sRGB. Embedded
  cursors use the color-managed scene path.
- Permission checks remain. Explicit captures wait for permission instead of
  saving an SDR permission-popup snapshot. Lock, resize, and source removal
  invalidate pending capture. Privacy filtering and suppression of surface
  frame feedback apply during the isolated render.
- A missing/failing permission-dialog helper denies capture instead of crashing
  or granting access.
- Each frame incurs full-scene rendering. Screen shaders/debug overlays are not
  an HDR export contract; existing fadeout snapshots may already be SDR.

**No explicit frame description means legacy SDR**, even with ten-bit buffers
or an HDR output. wlr-screencopy, toplevel-export, and portal behavior are
unchanged. When enabled, ext-image-copy additionally advertises RGB10 formats,
including for untagged SDR frames. Workspace legacy SDR scratch and explicit
FP16 scratch are independent. Never infer capture color from buffer depth,
output hints, or the streaming peer's display.

## Build and reproduce

```sh
nix develop -c cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_TESTS=ON
nix develop -c cmake --build build --target Hyprland hyprland_gtests color-managed-capture color-managed-source -j3
nix develop -c build/hyprland_gtests
# Installs the tests, capture probe, and native RGB10 source:
nix build .#hyprland-with-tests
```

Format C++ with **clang-format 23**: the release's older formatter cannot parse
`PackArguments` in `.clang-format`. Do not change the style file to hide this.

See [lab helpers](../debug-tools/hdr-capture/README.md). They create a private
runtime/D-Bus session and a headless Sway parent. The lab-only Aquamarine override
caps three parent-global bindings to advertised versions; normal packages retain
release-pinned Aquamarine. No physical outputs or system services are managed.
Bright reference patches must never be launched on the real desktop.

## Validation record (NVIDIA RTX 3090, driver 595.112.07)

### Native fixtures

`color-managed-source` creates a genuine SHM XBGR2101010 BT.2020/PQ surface,
neutral/R/G/B ramps, 0–10,000-nit patches, and an SDR child. It optionally creates
a controlled SDR cursor or locks the isolated session. The whole-frame checker
verifies pixels and the explicit frame description, not just encoder tags.

Previous artifacts in `/tmp/hh.0AiFm0/` record exact ramps, SHM/DMA-BUF agreement,
SDR white/gray, embedded cursor and private-window filtering. Lock, resize,
output removal, denied/pending permission, and missing-dialog failure were also
exercised. `/tmp/hh.peTXV2/validation.json` records the same exact RGB agreement
at 3440×1440; its short capture-only runs achieved approximately 141–144 fps.
Those are not sustained capture-to-encode or network/display benchmarks.

The initial mpv experiment in `/tmp/hh.AqlvAv/` retained HDR highlight patches,
but used eight-bit EGL storage. Native fixture validation supersedes it as the
precision gate; it is not evidence that mpv supplied ten-bit source pixels.

### Fresh packaged build and Stargaze integration

The fresh `hyprland-with-tests` package passed **868 tests**. Its installed source
and probe produced exact whole-frame SHM/DMA-BUF agreement again at 1024×512:
`/tmp/hh.C8eadc/validation.json`, raw pixels, contracts, and compositor logs.
The lab processes have been stopped.

The private Stargaze worktree `/projects/rust/stargaze-wayland` now provides
`--wayland-output Main` (DMA-BUF) and explicit `--wayland-shm` diagnostics. Portal
capture stays the default SDR backend. It chooses PQ/BT.2020 from the preferred
source description only as a request policy, then acknowledges an explicit
per-frame description. `--use-hdr false` requests genuine compositor-rendered
sRGB. There is no implicit HDR-to-SDR relabel or silent storage fallback.

Capture-to-NVENC tests pass for both backends using the freshly packaged
compositor. Three independent frames are retained past capture shutdown, then
imported/encoded and software-decoded: YUV420P10LE, limited BT.2020 NCL/PQ,
correct black/white, and >8-bit gradient precision. DMA-BUF uses the real
NVIDIA modifier and EGL/GL/CUDA import path. Explicit SDR and unsupported
compositor tests pass. Logs: `/tmp/stargaze-wayland-fresh-live.log` and
`/tmp/stargaze-wayland-unsupported.log`.

Stargaze fmt/check and all-target pedantic Clippy pass with no new capture/build
diagnostics (pre-existing warnings remain); nextest reports **286 passed,
32 skipped**. Its normal wrapped Nix server package also builds and rejects an
unknown output cleanly. These tests do not cover live peer presentation or a
long-running production session. Temporary artifacts are not durable release
pins.

## Sustained and photographic validation

The [machine-readable validation record](../debug-tools/hdr-capture/validation/2026-11-10.json)
retains revisions, source provenance, measurements, caveats, and temporary artifact
paths. The user authorized **unsigned local commits** pending later rebasing and
signing. Exact builds succeeded for compositor `b9df23ef7` (868 packaged tests)
and Stargaze `42036cd` (287 workspace tests, 34 hardware skips). Subsequent
Hyprland changes at `927759f52` are test-client/comparison helpers, not compositor
rendering changes; that revised client was compiled locally.

At 3440×1440, the native fixture → independent DMA-BUF → NVENC Main10 path
sustained **144.99 fps for 120 seconds** (17,399 encoded frames). FDs stayed at
76, RSS grew by 688 KiB, and p99 frame preparation was 4.686 ms. Phase-anchored
request deadlines removed accumulated wake-up jitter: the prior 120-second run
was 143.88 fps. Missed deadlines are discarded, not burst-replayed. The SHM
path remained correct but reached only **51.49 fps for 60 seconds**; it is not
the high-rate path. Post-run software decoding validates color, precision and
contiguous packets; full-frame native comparison gave Y/U/V PSNR 76.64/83.13/
86.92 dB with maximum errors 1/3/1 ten-bit codes.

A 4K/25-fps BT.2020/PQ AV1 photographic demo was played in mpv, with a separate
sRGB white/gray window over it. **Vulkan**, unlike the earlier OpenGL experiment,
selected `R16G16B16A16_SFLOAT` pass-through storage and an acknowledged Wayland
PQ description. Raw SHM and real DMA-BUF captures matched exactly. Every
background and SDR-window pixel matched expectations (160-nit white = PQ10 569,
gray = 415), including restoring the scene after moving the SDR window to an
isolated SDR output and back. Global window coordinates must account for output
positions; the reproduction fixes Main at `(0,0)`.

The paused Vulkan scene's decoded Main10 output measured **59.14/59.55/59.84 dB
Y/U/V PSNR**, with p99 error three codes. Raw snapshots before and after encoding
were identical. Tone-mapped reference/encoded PNGs were also inspected and are
retained in `/tmp/hh.AKRpkT/`; they do not certify a physical HDR display. The
photographic demo itself labels its variants Synthetic_SDR/Synthetic_HDR, so it
is application-playback evidence, not native-camera HDR or game validation.

Playing that application at 3440×1440 delivered **144.67 capture/encode fps over
60 seconds**, stable FDs, and approximately 10 MiB RSS growth. Its content is only
25 fps, not 145 unique application frames. A concurrent Nix server build adds
CPU contention to this run; the native 120-second paced run had no competing
package build. Resize, HDR→SDR output change, and output removal also terminate
the direct backend cleanly against the pinned compositor.

The endurance test requests one recovery IDR near its end and retains only
that suffix for offline decoding. Production uses an infinite GOP; an initial
test incorrectly retained the entire stream and exceeded its RSS budget. Fixing
the test bounded memory without changing production GOP policy. A premature
post-seek image comparison likewise correctly failed; paused-scene comparisons
must verify settled, identical snapshots.

These are **capture-to-encode** measurements, not network/display latency.
`queue_us` starts at capture delivery; preparation timing includes GPU conversion
and submission, while packet drain timing is not total hardware encoding time.
Both continuation labs (`/tmp/hh.AKRpkT`, `/tmp/hh.UsJYRm`) are stopped. No service,
physical output, or system configuration was changed.

## Remaining acceptance gates

- Native-camera HDR/game content, hours-long soak tests, device migration, and
  more complex source/output transitions. Minute-scale stability is not a soak.
- Sustained capture → network → AMD display at the target rate, including loss,
  reconnects, resource usage, and actual client presentation/calibration. There
  is no new end-to-end transport or panel-luminance result here.
- Per-frame DMA-BUF allocation remains deliberately independent. Pooling needs
  an encoder lease contract and separate evidence, not early storage reuse.
- Rebase/sign the local commits when the key is available and rebuild the final
  exact deployment pins. Obtain explicit approval before changing system or
  compositor pins, activating NixOS, or restarting the remote-desktop service.
