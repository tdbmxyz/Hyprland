#!/usr/bin/env python3
"""Validate the native RGB10 fixture, including every pixel and all 1024 codes.

Use --pair to compare real DMA-BUF readback to SHM, --cursor for the fixture's
16x16 SDR cursor at (900,450), or --black after applying the private-window rule.
"""

import argparse
import array
import json
import math
from pathlib import Path
import sys


def pq10(nits):
    power = (nits / 10000) ** (2610 / 16384)
    return round(((3424 / 4096 + 2413 / 128 * power) / (1 + 2392 / 128 * power)) ** (2523 / 32) * 1023)


def load(path):
    metadata = json.loads(Path(str(path) + ".json").read_text())
    contract = {"contract": "explicit-frame-image-description", "primaries": "bt2020", "transfer": "st2084-pq", "range": "full", "format": "xbgr2101010"}
    if any(metadata.get(key) != value for key, value in contract.items()):
        raise SystemExit(f"Unexpected color contract: {path}")
    width, height = metadata["width"], metadata["height"]
    if width < 1024 or height < 512 or metadata["stride"] != width * 4:
        raise SystemExit("Expected an unscaled fullscreen fixture at least 1024x512")
    pixels = array.array("I")
    pixels.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        pixels.byteswap()
    if pixels.itemsize != 4 or len(pixels) != width * height:
        raise SystemExit(f"Invalid frame size: {path}")
    return metadata, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frame", type=Path)
    parser.add_argument("--pair", type=Path)
    parser.add_argument("--cursor", action="store_true")
    parser.add_argument("--black", action="store_true")
    args = parser.parse_args()
    metadata, pixels = load(args.frame)
    width, height = metadata["width"], metadata["height"]
    patches = [pq10(nits) for nits in (0, 80, 160, 203, 400, 1000, 4000, 10000)]
    white = pq10(160)
    gray = pq10(160 * ((128 / 255 + 0.055) / 1.055) ** 2.4)
    errors = []
    max_error = 0
    for y in range(height):
        for x in range(width):
            ramp, band = x % 1024, y % 512
            expected = (0, 0, 0)
            if not args.black:
                if band < 64:
                    expected = (ramp,) * 3
                elif band < 128:
                    expected = (ramp, 0, 0)
                elif band < 192:
                    expected = (0, ramp, 0)
                elif band < 256:
                    expected = (0, 0, ramp)
                elif band < 320:
                    expected = (patches[ramp // 128],) * 3
                if 320 <= y < 384 and x < 512:
                    expected = (white if x < 256 else gray,) * 3
                if args.cursor and 900 <= x < 916 and 450 <= y < 466:
                    expected = (white,) * 3
            actual = tuple((pixels[y * width + x] >> shift) & 1023 for shift in (0, 10, 20))
            error = max(abs(a - b) for a, b in zip(actual, expected))
            max_error = max(max_error, error)
            # Pure ten-bit ramps must be exact; converted SDR/patch values can
            # vary by one code due to transfer-function/half-float rounding.
            tolerance = 0 if args.black or band < 256 else 1
            if error > tolerance and len(errors) < 12:
                errors.append({"x": x, "y": y, "expected": expected, "actual": actual})
    result = {"width": width, "height": height, "backend": metadata["backend"], "max_code_error": max_error, "errors": errors}
    if not args.black:
        result["ramp_unique_codes"] = [len({(pixels[y * width + x] >> (channel * 10)) & 1023 for x in range(1024)}) for y, channel in ((32, 0), (96, 0), (160, 1), (224, 2))]
        if result["ramp_unique_codes"] != [1024] * 4:
            errors.append({"reason": "ten-bit code values collapsed"})
    if args.pair:
        other_meta, other = load(args.pair)
        if (other_meta["width"], other_meta["height"]) != (width, height) or other_meta["cursor"] != metadata["cursor"]:
            raise SystemExit("Frame-pair geometry/cursor contracts differ")
        result["pair_rgb_mismatches"] = sum((a & 0x3FFFFFFF) != (b & 0x3FFFFFFF) for a, b in zip(pixels, other))
        if result["pair_rgb_mismatches"]:
            errors.append({"reason": "SHM and DMA-BUF RGB pixels differ"})
    print(json.dumps(result, indent=2))
    if errors:
        raise SystemExit("Native desktop capture validation failed")


if __name__ == "__main__":
    main()
