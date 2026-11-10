# HDR desktop capture — working state

Private Hyprland fork (official **v0.57.2** base) plus Stargaze integration.
**No upstream interaction, deployment, or system activation.**

## Worktrees and safety

- Hyprland: `/projects/Hyprland`, `feature/hdr-capture-v0.57.2`.
- Stargaze: `/projects/rust/stargaze-wayland`, `feature/wayland-hdr-capture`.
- **Leave `/projects/rust/stargaze` untouched**; another session uses it.
- Do not activate NixOS, replace the compositor, or restart
  `remote-desktop-Main.service` without separate approval. That user service
  remains **inactive**, not restarted. Keep the existing endpoint / port 9002
  untouched.
- Always select isolated Wayland/IPC endpoints explicitly. **All labs started
  by these continuations are stopped.** No physical outputs were used.

## Local unsigned commits are now authorized

The user explicitly approved unsigned commits; they intend to rebase/sign when
GPG is available. Commits use a one-command signing override; persistent Git
signing configuration is unchanged. Nothing was pushed.

Hyprland code commits:

- `648b9b5ad`: permission-dialog failure denies capture instead of crashing or
  granting access.
- `b9df23ef7d397a84016237eaaec0ba62964cd192`: opt-in color-described RGB10 scene
  capture, protocol/tests, native source and GPU readback tools.
- `927759f522435a72e20620f71c7a25f45738e4ba`: independent SDR window fixture and
  full-frame composition/Main10 comparison helpers. Only test tools changed;
  the compositor rendering implementation remains the b9df23ef7 version.

Stargaze code commits:

- `374d22a3a5fa4bc43095d4eef7a0ab0ed3270583`: direct Wayland backend, protocol
  build wiring, dependencies, explicit CLI source selection and Main10 tests.
- `2ea95a5`: preserve scheduled pacing phase instead of accumulating wake-up
  jitter; discard missed slots rather than burst to catch up. Includes a unit
  regression test.
- `42036cd6abbe2f725ea3b36be60afca9c5063040`: isolated endurance and output-change
  tests with JSON/resource samples and final HEVC/decoded-frame artifacts.

Subsequent documentation commits contain this handoff and numerical record.

## Implemented contract

- Hyprland's experimental capture extension defaults **off**. Explicit
  BT.2020/PQ RGB10 capture uses its own unscaled-linear FP16 scene; ordinary
  captures remain SDR. All 1,024 codes survive exactly.
- `--wayland-output Main` selects direct capture; portal remains default SDR.
  `--wayland-shm` explicitly chooses the slower RGB10 diagnostic path.
- Preferred color is only request policy. A ready description is applied to
  each frame; no HDR inference from buffer depth/output/peer hints.
- `--use-hdr false` requests genuine compositor-rendered sRGB.
- Each DMA-BUF owns independent storage/FDs through encoder lifetime, including
  after capture shutdown. No unsafe buffer pooling/reuse was introduced.
- Resize, preferred-color changes and source removal stop the fixed-mode path.

## Exact builds and software checks

Successful immutable local Git-pin builds:

- Compositor b9df23ef7:
  `/nix/store/ns6g5aazwkxyaahkwd278xy80br52wyh-hyprland-0.57.2+date=2026-11-10_b9df23e`
  Packaged **868 unit tests passed**. Logs: `/tmp/hdr-pinned-package.log`,
  `/tmp/hdr-pinned-package-tests.log`. The newer SDR fixture at 927759f52 was
  compiled locally, not rebuilt into this compositor package.
- Server 42036cd:
  `/nix/store/68crpc2vhxhizigrhml6cz56p6d23js0-stargaze-server-1.5.0`
  Logs: `/tmp/stargaze-paced-package.log`; help/closed-endpoint startup checks
  pass without opening a stream.
- Stargaze fmt/check and all-target pedantic Clippy: no new capture/build
  diagnostics; pre-existing warnings remain. Nextest: **287 passed, 34 skipped**.
  Log: `/tmp/stargaze-endurance-final-quality.log`.
- C++ uses clang-format 23; the release's older formatter cannot parse its style.

## Automated hardware/non-visual results

Durable numbers, provenance, limitations, and artifact locations:
**`debug-tools/hdr-capture/validation/2026-11-10.json`**.

### 3440×1440 native fixture → NVENC Main10

- Full-frame SHM/real DMA-BUF captures agree exactly; all neutral/R/G/B codes
  preserved. `/tmp/hh.UsJYRm/validation.json`.
- After pacing fix: **144.988 fps for 120 seconds**, 17,399 frames. FDs stable
  at 76; RSS 460,468 → 461,156 KiB; p99 conversion/submission 4.686 ms.
- Before pacing fix: 143.877 fps for 120 seconds. The fix removes accumulated
  scheduling drift, not rendering precision or encoder quality.
- SHM: **51.486 fps over 60 seconds**, correct pixels/color and stable resources.
  It is diagnostic, not the high-rate path.
- Native full-raster decoded Main10/reference PSNR: **76.64/83.13/86.92 dB** Y/U/V;
  maximum differences 1/3/1 ten-bit codes.
- Logs: `/tmp/stargaze-native-dma-paced-120.log`,
  `/tmp/stargaze-native-shm-60.log`. Raw/JSON/HEVC/YUV and GPU CSV:
  `/tmp/hh.UsJYRm/`.

### Photographic playback and independent SDR window

- Downloaded 4K/25-fps AV1 BT.2020/PQ photographic demo. URL/SHA256 are recorded
  in the numerical JSON; media is not committed. It labels authored variants
  Synthetic_SDR/Synthetic_HDR: **not native-camera HDR or game validation**.
- OpenGL mpv still uses eight-bit EGL. **Vulkan mpv works with FP16 pass-through
  storage and an acknowledged PQ Wayland description**, providing a preserving
  real application path. Confirm final swapchain selection after the HDR frame,
  not the initial RGB10/sRGB startup configuration.
- Paused 1920×1080 Vulkan scene plus separate sRGB white/gray window:
  SHM/DMA-BUF raw captures identical; every background/overlay pixel correct.
  SDR white 160 nits = PQ10 569; gray = 415.
- Main10 versus full-frame matrix reference: **59.14/59.55/59.84 dB** Y/U/V,
  p99 error three codes. Raw captures before/after encoding match byte-for-byte.
- Tone-mapped previews were inspected:
  `/tmp/hh.AKRpkT/vulkan-reference-sdr.png` and `vulkan-encode-sdr.png`.
  These are SDR comparison aids, not physical HDR panel measurements.
- Actual playback at 3440×1440: **144.665 capture/encode fps over 60 seconds**,
  stable 76 FDs, ~10 MiB RSS increase. A server Nix build was sharing CPU during
  this run. The application source is only 25 fps, not 145 unique frames.
- Moving the SDR window to a private SDR output restores the HDR background
  exactly; returning restores the mixed scene. Use explicit Main `(0,0)` and
  correct global coordinates: automatic monitor placement initially moved Main.
- Direct capture termination on resize/HDR→SDR/output removal passes against
  the pinned compositor, also with Vulkan playback. Log:
  `/tmp/stargaze-pinned-source-changes.log`.

### Test-harness pitfalls caught and fixed

- Production intentionally uses an **infinite GOP**. Initially retaining every
  packet for post-run decoding falsely grew test RSS. The harness now requests
  ONE late recovery IDR and retains only its final suffix. A repeat 60-second
  dynamic run stays stable; production GOP behavior was not changed.
- Seeking paused mpv returns before rendering necessarily settles. An early
  comparison correctly failed; require identical raw snapshots before/after
  encoding for a valid static-scene comparison.
- Packet timing begins at capture delivery; it is not full scene-render,
  hardware encoder, network, or display latency. FDs/RSS are process-local.

## Next steps (not blocked by signing)

1. Native-camera HDR/game content and hours-long soak testing; device migration
   and additional complex output transitions. Minute-scale tests are not a soak.
2. Capture → transport/loss/reconnect → AMD-client presentation at target rate,
   including client readback and calibration. No new network/display or panel
   luminance result is claimed. Avoid launching bright content on a physical
   output without separate approval; the user cannot visually assist currently.
3. Rebase/sign when the key is available and rebuild final exact deployment pins.
   Existing exact unsigned pins are already built, so development is not blocked.
4. Deployment/system/service changes still require separate approval.

Reproduction: `debug-tools/hdr-capture/README.md`, `docs/hdr-capture.md`, and
Stargaze `docs/hdr-streaming.md`. Its active plan remains
`plans/wayland-hdr-capture.md` (not merged/deployed).
