#!/usr/bin/env python3
"""Compare a static RGB10 capture to a decoded Main10 YUV420P10LE frame.

The reference matrix is applied to existing PQ code values (not linear light).
Chroma is a 2x2 box average, matching Stargaze's BT.2020-NCL P010 converter.
Input must be the SAME paused scene, with no cursor or geometry changes.
This tests capture-to-encode fidelity, not panel luminance or source mastering.
"""

import argparse
import array
import json
import math
from pathlib import Path
import sys


def samples(path, kind):
    result = array.array(kind)
    result.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        result.byteswap()
    return result


def luma(word):
    return (0.2627 * (word & 1023) + 0.6780 * ((word >> 10) & 1023) + 0.0593 * ((word >> 20) & 1023)) / 1023


def reference(pixels, width, height):
    y = array.array("H", (round(64 + 876 * luma(word)) for word in pixels))
    u, v = array.array("H"), array.array("H")
    for row in range(0, height, 2):
        for column in range(0, width, 2):
            block = [pixels[(row + dy) * width + column + dx] for dy in (0, 1) for dx in (0, 1)]
            red = sum(word & 1023 for word in block) / (4 * 1023)
            blue = sum((word >> 20) & 1023 for word in block) / (4 * 1023)
            value = sum(luma(word) for word in block) / 4
            u.append(round(512 + 896 * (blue - value) / 1.8814))
            v.append(round(512 + 896 * (red - value) / 1.4746))
    return y, u, v


def statistics(actual, expected):
    errors = [int(a) - int(b) for a, b in zip(actual, expected)]
    absolute = sorted(abs(value) for value in errors)
    mse = sum(value * value for value in errors) / len(errors)
    return {
        "psnr_db": 10 * math.log10(1023 * 1023 / mse) if mse else None,
        "mean_bias_codes": sum(errors) / len(errors),
        "p99_error_codes": absolute[(len(absolute) - 1) * 99 // 100],
        "max_error_codes": absolute[-1],
        "exact": mse == 0,
    }


def self_test():
    # Known limited BT.2020-NCL codes: black, white, and saturated primaries.
    for rgb, expected in [((0, 0, 0), (64, 512, 512)), ((1023,) * 3, (940, 512, 512)),
                          ((1023, 0, 0), (294, 387, 960)), ((0, 1023, 0), (658, 189, 100)),
                          ((0, 0, 1023), (116, 960, 476))]:
        r, g, b = rgb
        planes = reference([r | (g << 10) | (b << 20)] * 4, 2, 2)
        assert tuple(plane[0] for plane in planes) == expected
    assert statistics([64, 940], [64, 940])["exact"]
    assert statistics([65, 939], [64, 940])["max_error_codes"] == 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, nargs="?")
    parser.add_argument("decoded", type=Path, nargs="?")
    parser.add_argument("--reference-yuv", type=Path)
    parser.add_argument("--min-psnr", type=float, default=40)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.capture or not args.decoded:
        parser.error("capture and decoded frame paths are required")
    metadata = json.loads(Path(str(args.capture) + ".json").read_text())
    expected = {"contract": "explicit-frame-image-description", "primaries": "bt2020",
                "transfer": "st2084-pq", "range": "full", "format": "xbgr2101010", "cursor": False}
    if any(metadata.get(key) != value for key, value in expected.items()):
        raise SystemExit("Unexpected RGB10 capture contract")
    width, height = metadata["width"], metadata["height"]
    if width % 2 or height % 2 or metadata["stride"] != width * 4:
        raise SystemExit("Expected tightly packed even geometry")
    pixels = samples(args.capture, "I")
    decoded = samples(args.decoded, "H")
    if len(pixels) != width * height or len(decoded) != width * height * 3 // 2:
        raise SystemExit("Invalid raw frame length")
    planes = reference(pixels, width, height)
    offset, results = 0, {}
    for name, plane in zip(("y", "u", "v"), planes):
        results[name] = statistics(decoded[offset:offset + len(plane)], plane)
        offset += len(plane)
    if args.reference_yuv:
        with args.reference_yuv.open("wb") as output:
            for plane in planes:
                if sys.byteorder != "little":
                    plane.byteswap()
                plane.tofile(output)
    print(json.dumps({"width": width, "height": height, "planes": results}, indent=2))
    if any(not value["exact"] and value["psnr_db"] < args.min_psnr for value in results.values()):
        raise SystemExit("Capture/Main10 fidelity below threshold (also check scene synchronization)")


if __name__ == "__main__":
    main()
