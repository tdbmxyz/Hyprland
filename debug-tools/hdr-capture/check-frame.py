#!/usr/bin/env python3
"""Check a captured desktop displaying make-reference.py's fullscreen PQ clip.

This checks highlight preservation, not full ten-bit precision, calibrated panel
luminance, arbitrary desktop content, cursor correctness or DMA-BUF agreement.
"""

import argparse
import json
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frame", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--tolerance", type=int, default=2)
    args = parser.parse_args()
    metadata = json.loads(Path(str(args.frame) + ".json").read_text())
    reference = json.loads(args.reference.read_text())
    expected_contract = {
        "contract": "explicit-frame-image-description", "primaries": "bt2020",
        "transfer": "st2084-pq", "range": "full", "format": "xbgr2101010",
    }
    if any(metadata.get(key) != value for key, value in expected_contract.items()):
        raise SystemExit("Unexpected capture contract")
    width, height, stride = (metadata[key] for key in ("width", "height", "stride"))
    if (width, height) != (reference["width"], reference["height"]) or stride != width * 4:
        raise SystemExit("Expected unscaled fullscreen reference and tightly packed pixels")
    pixels = args.frame.read_bytes()
    if len(pixels) != stride * height or args.tolerance < 0:
        raise SystemExit("Invalid frame length or tolerance")
    samples = []
    for i, expected in enumerate(reference["pq10"]):
        x, y = (2 * i + 1) * width // 16, height // 2
        value = struct.unpack_from("<I", pixels, y * stride + x * 4)[0]
        rgb = [(value >> shift) & 1023 for shift in (0, 10, 20)]
        samples.append({
            "nits": reference["nits"][i], "expected": expected, "actual": rgb,
            "max_code_error": max(abs(channel - expected) for channel in rgb),
        })
    print(json.dumps(samples, indent=2))
    if max(sample["max_code_error"] for sample in samples) > args.tolerance:
        raise SystemExit("HDR patches did not survive compositor capture")


if __name__ == "__main__":
    main()
