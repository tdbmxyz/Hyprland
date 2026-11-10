#!/usr/bin/env python3
"""Write neutral PQ reference pixels for lossless playback by a real HDR client."""

import argparse
import json
from pathlib import Path
import struct

NITS = (0, 80, 160, 203, 400, 1000, 4000, 10000)
WIDTH, HEIGHT = 640, 360


def pq(nits):
    power = (nits / 10000) ** (2610 / 16384)
    return ((3424 / 4096 + 2413 / 128 * power) / (1 + 2392 / 128 * power)) ** (2523 / 32)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    row = [round(pq(NITS[min(7, x * 8 // WIDTH)]) * 65535) for x in range(WIDTH)]
    plane = b"".join(struct.pack("<H", value) for value in row) * HEIGHT
    # gbrp16le: identical full-range neutral G, B and R planes.
    (args.directory / "hdr-reference.gbrp16le").write_bytes(plane * 3)
    (args.directory / "hdr-reference.json").write_text(json.dumps({
        "width": WIDTH, "height": HEIGHT, "nits": NITS,
        "pq10": [round(pq(nits) * 1023) for nits in NITS],
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
