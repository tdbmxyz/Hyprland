#!/usr/bin/env python3
"""Check an SDR white/gray floating fixture over a paused HDR background.

Both inputs must be explicit PQ RGB10 captures with no cursor. The fixture's
window geometry is explicit; pixels outside it must retain the background.
"""

import argparse
import array
import json
from pathlib import Path
import sys


def load(path):
    metadata = json.loads(Path(str(path) + ".json").read_text())
    contract = {"primaries": "bt2020", "transfer": "st2084-pq", "range": "full", "format": "xbgr2101010", "cursor": False}
    if any(metadata.get(key) != value for key, value in contract.items()):
        raise SystemExit("Expected an explicit cursor-free HDR capture")
    width, height = metadata["width"], metadata["height"]
    pixels = array.array("I")
    pixels.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        pixels.byteswap()
    if len(pixels) != width * height or metadata["stride"] != width * 4:
        raise SystemExit("Invalid packed capture geometry")
    return width, height, pixels


def pq10(nits):
    power = (nits / 10000) ** (2610 / 16384)
    return round(((3424 / 4096 + 2413 / 128 * power) / (1 + 2392 / 128 * power)) ** (2523 / 32) * 1023)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("background", type=Path)
    parser.add_argument("overlay", type=Path)
    parser.add_argument("--box", type=int, nargs=4, default=[64, 64, 512, 128], metavar=("X", "Y", "W", "H"))
    parser.add_argument("--sdr-white", type=float, default=160)
    args = parser.parse_args()
    width, height, background = load(args.background)
    w, h, overlay = load(args.overlay)
    if (w, h) != (width, height):
        raise SystemExit("Capture geometry changed")
    x, y, w, h = args.box
    if x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > width or y + h > height:
        raise SystemExit("Invalid overlay rectangle")
    white = pq10(args.sdr_white)
    gray = pq10(args.sdr_white * ((128 / 255 + 0.055) / 1.055) ** 2.4)
    errors = []
    maxima = {"background": 0, "sdr_white": 0, "sdr_gray": 0}
    for row in range(height):
        for column in range(width):
            index = row * width + column
            region = "background"
            expected = tuple((background[index] >> shift) & 1023 for shift in (0, 10, 20))
            if x <= column < x + w and y <= row < y + h:
                region = "sdr_white" if column < x + w // 2 else "sdr_gray"
                expected = (white if region == "sdr_white" else gray,) * 3
            actual = tuple((overlay[index] >> shift) & 1023 for shift in (0, 10, 20))
            error = max(abs(a - b) for a, b in zip(actual, expected))
            maxima[region] = max(maxima[region], error)
            if error > 1 and len(errors) < 8:
                errors.append({"x": column, "y": row, "region": region, "expected": expected, "actual": actual})
    print(json.dumps({"width": width, "height": height, "white_pq10": white, "gray_pq10": gray,
                      "maximum_code_errors": maxima, "errors": errors}, indent=2))
    if errors:
        raise SystemExit("Mixed SDR/HDR composition did not match")


if __name__ == "__main__":
    main()
