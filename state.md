# HDR desktop capture — working state

Personal Hyprland fork (official **v0.57.2** base) plus Stargaze integration.
Personal-origin pushes and NixOS configuration preparation are authorized.
**No upstream interaction or agent-performed activation/service restart.**
The user has successfully tested the deployed HDR pipeline; the new blur-flash
shader fix is validated locally but still needs deployment.

## Worktrees and safety

- Hyprland: `/projects/Hyprland`, long-lived **`hdr`** branch, published to
  `git@github.com:tdbmxyz/Hyprland.git` (verified personal fork).
- Primary Stargaze: `/projects/rust/stargaze`, **`feature/hdr-safe-streaming`**.
  The user authorized integration here; the direct backend was merged with a
  signed no-fast-forward merge and published to `tdbmxyz/stargaze`.
- `/projects/rust/stargaze-wayland` remains a supplementary worktree on
  `feature/wayland-hdr-capture`, no longer the deployment target.
- Do not activate NixOS, replace the compositor, or restart
  `remote-desktop-Main.service` without separate approval. The launcher is a
  **system** service running as tibo; the stream is the **user** service
  `stargaze-server.service`. The user activated/tested Zeus and Athena with
  `nixos-rebuild test`; both live services were active during the investigation.
  The agent did not restart them. Keep the existing endpoint / port 9002 untouched.
- Always select isolated Wayland/IPC endpoints explicitly. **All labs started
  by these continuations are stopped.** No physical outputs were used.

## Signed publication and branch workflow

The formerly unsigned task commits have now been re-signed with the available
OpenPGP key; signatures verify good, and source trees are unchanged by signing.
The original SHAs below remain the historical measurement provenance. Signed
Hyprland equivalents: `793b957f2` (permissions), `6bddb07ba` (capture),
`d8132d670` (tools), `31eb20b28` (evidence). Signed Stargaze equivalents:
`1ebfe20`, `0a91559`, `75be560`, `8f1c209`.

Maintain ONE Hyprland `hdr` branch, rebased onto each new official release, with
signed checkpoint tags preserving tested versions. Publish rebases using
`--force-with-lease`, not unguarded force pushes. Nix locks retain exact revisions.
Hyprland Actions on the personal fork are optional: the user explicitly prefers
NixOS builds plus separately run tests, not enabling Actions as a prerequisite.

Stargaze integration is published at `d9883fd` on `feature/hdr-safe-streaming`.
Its push CI **42982066158** passed (fmt, all-target Clippy and nextest). The
completed direct-backend plan was removed; remaining validation is in
`plans/hdr-safe-streaming.md` in the primary Stargaze worktree. Existing untracked
`result-hdr-*` symlinks there were left untouched.

`/etc/nixos` on `feature/zeus-headless-hdr` selects the fork's `hdr` branch and
Stargaze's integrated branch, locked exactly. Prior to the blur fix, published
configuration `2678133c2478f6d48b19efd362e7deab08316de3` pins Hyprland `a21cd8561`
and Stargaze `d9883fd`. Keep `hyprland.inputs.nixpkgs.follows = "nixpkgs"` for
system Mesa/glibc compatibility. Main opts into direct Wayland capture; other
presets keep the portal default. The extension is only enabled for hosts/presets
selecting that backend. Screencopy permission is scoped to the pinned Stargaze
executable; global enforcement stays enabled. The launcher passes the selected
compositor's explicit Wayland/IPC endpoints into the user service. Launcher mock
tests pass, zeus's system derivation evaluates, and both selected packages built.
The user subsequently activated/tested the pipeline successfully. Preparing an
updated Hyprland pin and building it is authorized, but further activation or
service restart still requires separate approval. Actual lock/build results
live in that repository; Athena's animations/calibration remain unchanged.

Historical Hyprland code commits (before re-signing):

- `648b9b5ad`: permission-dialog failure denies capture instead of crashing or
  granting access.
- `b9df23ef7d397a84016237eaaec0ba62964cd192`: opt-in color-described RGB10 scene
  capture, protocol/tests, native source and GPU readback tools.
- `927759f522435a72e20620f71c7a25f45738e4ba`: independent SDR window fixture and
  full-frame composition/Main10 comparison helpers. Only test tools changed;
  the compositor rendering implementation remains the b9df23ef7 version.

Historical Stargaze code commits (before re-signing):

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

## User streaming validation (2026-11-11)

The user ran `nixos-rebuild test` on Zeus and Athena and streamed for about
**245.7 seconds**, reporting good visual rendering. Confirmed: 3440×1440,
nominal 145 fps, ten-bit limited-range BT.2020/PQ, approximately **143.5 fps
received / 142.9 fps decoded**, VAAPI, actual EGL back-buffer precision
`(10,10,10)`, acknowledged Wayland PQ image description, and P010 two-plane
DMA-BUF import. Zeus journals confirm GPU DMA-BUF capture, RGBA16 CUDA conversion
and NVENC P010 Main10; no observed CPU fallback or PTX errors.

Logs: `/tmp/stargaze-zeus-hdr-user-session.log` and
`/tmp/stargaze-zeus-hdr-launcher-session.log`. Startup losses and a queue spike
are not evidence of persistent network trouble. Near-zero reported encode time
is misleading due to timer placement; decoder backpressure also counts some
recovery discards. This validates the pipeline and short-session presentation,
not physical panel luminance or hours-long stability.

## HDR window/menu flash fix (2026-11-11)

Opening apps and Steam dropdowns briefly flashed brightly before fading normally.
An isolated reproduction found `blurprepare.glsl` unconditionally calling
`toNit` on the HDR capture's `CM_TRANSFER_FUNCTION_LINEAR` working buffer, which
already stores absolute nits. This multiplied luminance again, typically by
10,000. Fractional-opacity windows invoke blur; full opacity skips that path.
The fix skips `toNit` for unscaled linear input, matching `cm_helpers.glsl`.
No animation disabling, config option change, or Kawase C++ change is needed.

New regression: `debug-tools/hdr-capture/test-blur-fade.py`, using its own private
headless lab, native HDR background and SDR foreground, PQ RGB10 capture,
six opacity steps × blur off/on, four 8×8 sample regions/case, tolerance two codes.
Named window rules apply opacity; `hyprctl setprop` returned `unknown request`
with exit status zero and must not be used here. Cleanup runs in `finally`.

- Before: **5/12 failed**, all fractional-opacity blur cases. 160-nit white
  expected PQ10 569, actual 1019 at blur/opacity 0.25, back to 569 at full opacity.
- After: **12/12 passed**, maximum error one code. Fixed compositor and unit-test
  targets built; **868 unit tests passed**.
- Durable record: `debug-tools/hdr-capture/validation/2026-11-11-blur-fade.json`.
- Full reports: `/tmp/hh.HHwxo6/blur-validation.json` (before),
  `/tmp/hh.mlhK51/blur-validation.json` (after); manual reproduction `/tmp/hh.CPxw1Y`.
  All labs are stopped. Logs: `/tmp/hdr-blur-{fixed-build,unit-build,unit-tests,fixed-regression,unfixed-regression}.log`.

**Not yet deployed.** Exact Steam/XWayland menu behavior still needs a recheck
in a fresh affected session after deployment. No Athena/client/Stargaze update
is needed for this shader fix. Existing live sessions were not modified.

## Next steps (not blocked by signing)

1. Sign/publish the shader, regression, evidence and docs on the personal `hdr`
   branch; do not enable fork Actions. Update only the NixOS Hyprland input,
   keep Stargaze `d9883fd`, and build the configuration-selected compositor.
   Sign/push the lock/docs; preserve the exact previous lock for rollback.
2. Obtain separate activation/restart approval before deploying the new
   compositor and starting a fresh affected session, then recheck Steam menus.
3. Native-camera HDR/game content and hours-long soak testing; device migration,
   additional complex output transitions, transport/loss/reconnect tests and
   client calibration remain. Minute-scale tests are not a soak or a physical
   panel-luminance measurement. No bright physical-output patterns without
   separate approval.

Reproduction: `debug-tools/hdr-capture/README.md`, `docs/hdr-capture.md`, and
Stargaze `docs/hdr-streaming.md`. Its active plan remains
`plans/hdr-safe-streaming.md` (direct backend integrated and user-tested).
