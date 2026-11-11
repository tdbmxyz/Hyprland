#!/usr/bin/env python3
"""Isolated GPU regression: fading SDR windows must not flash over HDR content.

Starts/stops its own private headless lab. Never uses the inherited desktop's
Wayland/IPC endpoint. Requires the built native source/capture clients and Nix.
"""

import argparse
import json
import os
import struct
import subprocess
import time
from pathlib import Path


def pq10(nits):
    power = (nits / 10000) ** (2610 / 16384)
    return round(
        ((3424 / 4096 + 2413 / 128 * power) / (1 + 2392 / 128 * power)) ** (2523 / 32)
        * 1023
    )


def wait_ready(process, log):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Fixture exited; inspect {log}")
        if "Ready:" in log.read_text():
            return
        time.sleep(0.05)
    raise RuntimeError(f"Fixture timed out; inspect {log}")


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hyprland", type=Path, default=repo / "build-hdr/Hyprland")
    parser.add_argument("--tools", type=Path, default=repo / "build-hdr/hyprtester")
    args = parser.parse_args()
    env = dict(os.environ, HYPRLAND_BINARY=str(args.hyprland.resolve()))
    started = subprocess.run(
        [str(repo / "debug-tools/hdr-capture/start-lab.sh"), "1024", "512", "60"],
        env=env,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    lab = Path(started.stdout.strip())
    if (
        lab.parent != Path("/tmp")
        or not lab.name.startswith("hh.")
        or lab.is_symlink()
        or lab.stat().st_uid != os.getuid()
    ):
        raise RuntimeError(f"Unexpected private runtime: {lab}")
    print(f"Evidence: {lab}", flush=True)
    try:
        (lock,) = lab.glob("hypr/*/hyprland.lock")
        display = lock.read_text().splitlines()[1]
        env.update(
            XDG_RUNTIME_DIR=str(lab),
            WAYLAND_DISPLAY=display,
            HYPRLAND_INSTANCE_SIGNATURE=lock.parent.name,
            HDR_CAPTURE_LAB="1",
        )
        for name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "DISPLAY"):
            env.pop(name, None)
        source = args.tools.resolve() / "color-managed-source"
        capture = args.tools.resolve() / "color-managed-capture"

        def control(lua):
            result = subprocess.run(
                ["hyprctl", "eval", lua],
                env=env,
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            )
            if result.stdout.strip() != "ok":
                raise RuntimeError(result.stdout)

        def spawn(name, arguments):
            log = lab / f"{name}.log"
            with log.open("w") as output:
                process = subprocess.Popen(
                    [str(source), *arguments],
                    env=env,
                    stdin=subprocess.DEVNULL,
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
            (lab / f"{name}.pid").write_text(f"{process.pid}\n")
            wait_ready(process, log)

        spawn("source", [])
        control(
            "hl.window_rule({name='blur-test-window',match={class='^hdr-sdr-reference$'},float=true,size={512,64},move={256,256}})"
        )
        spawn("probe", ["--sdr"])
        # Neutral blur adjustments isolate luminance conversion from the user's
        # chosen artistic brightness/contrast/noise. Both windows have fixed size.
        control(
            "hl.config({decoration={blur={size=4,passes=1,noise=0,contrast=1,brightness=1,vibrancy=0,new_optimizations=false}}})"
        )
        points = [
            (320, 160, 160),
            (448, 203, 160),
            (576, 400, 160 * ((128 / 255 + 0.055) / 1.055) ** 2.4),
            (704, 1000, 160 * ((128 / 255 + 0.055) / 1.055) ** 2.4),
        ]
        results = []
        failures = []
        for blur in (False, True):
            control(
                f"hl.config({{decoration={{blur={{enabled={str(blur).lower()}}}}}}})"
            )
            for alpha in (0.1, 0.25, 0.5, 0.75, 0.9, 1):
                control(
                    f"hl.window_rule({{name='blur-test-opacity',match={{class='^hdr-sdr-reference$'}},opacity='{alpha} {alpha}'}})"
                )
                raw = lab / f"blur-{blur}-alpha-{alpha}.raw"
                with raw.with_suffix(".log").open("w") as log:
                    subprocess.run(
                        [str(capture), "Main", str(raw), "--no-cursor"],
                        env=env,
                        check=True,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        timeout=15,
                    )
                metadata = json.loads(Path(str(raw) + ".json").read_text())
                contract = {
                    "primaries": "bt2020",
                    "transfer": "st2084-pq",
                    "range": "full",
                    "format": "xbgr2101010",
                    "width": 1024,
                    "height": 512,
                    "stride": 4096,
                    "cursor": False,
                }
                if any(metadata.get(key) != value for key, value in contract.items()):
                    raise RuntimeError(f"Unexpected capture contract: {metadata}")
                pixels = raw.read_bytes()
                samples = []
                maximum = 0
                for x, background, foreground in points:
                    expected = pq10(background * (1 - alpha) + foreground * alpha)
                    pixel = struct.unpack_from("<I", pixels, (288 * 1024 + x) * 4)[0]
                    samples.append(
                        [int((pixel >> shift) & 1023) for shift in (0, 10, 20)]
                    )
                    for row in range(284, 292):
                        for column in range(x - 4, x + 4):
                            word = struct.unpack_from(
                                "<I", pixels, (row * 1024 + column) * 4
                            )[0]
                            maximum = max(
                                maximum,
                                *(
                                    abs(((word >> shift) & 1023) - expected)
                                    for shift in (0, 10, 20)
                                ),
                            )
                result = {
                    "blur": blur,
                    "alpha": alpha,
                    "rgb_samples": samples,
                    "maximum_pq10_error": maximum,
                }
                results.append(result)
                if maximum > 2:
                    failures.append(result)
        report = {
            "hyprland": str(args.hyprland.resolve()),
            "samples_per_case": 256,
            "tolerance_pq10_codes": 2,
            "results": results,
            "failures": failures,
        }
        (lab / "blur-validation.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
        if failures:
            raise SystemExit("Blur/fade luminance regression detected")
    finally:
        subprocess.run(
            [str(repo / "debug-tools/hdr-capture/stop-lab.sh"), str(lab)], check=True
        )


if __name__ == "__main__":
    main()
