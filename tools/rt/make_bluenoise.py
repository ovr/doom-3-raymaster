# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.

"""Writes neo/renderer/BlueNoise.h: two 64 x 64 void-and-cluster textures (Ulichney 1993), interleaved.

    python tools/rt/make_bluenoise.py [--stats]

Every run gives the same bytes: the seeds are fixed, the energies are integers and a tie takes the lowest index.
--stats prints what the test Noise_Table holds the table to.
"""
import math
import sys
from pathlib import Path

SIZE = 64
TEXELS = SIZE * SIZE
SIGMA = 1.9
REACH = 8                      # the kernel's half width: below 2^-12 of its peak beyond
ONE = 1 << 24
INITIAL = TEXELS // 10         # the points of the first pattern
SEEDS = (0x5EED0001, 0x5EED0002)

KERNEL = [(dx, dy, round(math.exp(-(dx * dx + dy * dy) / (2.0 * SIGMA * SIGMA)) * ONE))
          for dy in range(-REACH, REACH + 1) for dx in range(-REACH, REACH + 1)]


class Lcg:
    def __init__(self, seed):
        self.state = seed

    def below(self, limit):
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
        return (self.state >> 33) % limit


def splat(energy, at, sign):
    x0, y0 = at % SIZE, at // SIZE
    for dx, dy, weight in KERNEL:
        energy[((y0 + dy) % SIZE) * SIZE + (x0 + dx) % SIZE] += sign * weight


def tightest(energy, pattern, value):
    """The point of `value` with the most of its kind around it."""
    best, at = -1, -1
    for i in range(TEXELS):
        if pattern[i] == value and energy[i] > best:
            best, at = energy[i], i
    return at


def emptiest(energy, pattern, value):
    """The point that is not `value` with the least of `value` around it."""
    best, at = None, -1
    for i in range(TEXELS):
        if pattern[i] != value and (best is None or energy[i] < best):
            best, at = energy[i], i
    return at


def ranks(seed):
    rng = Lcg(seed)
    pattern = [0] * TEXELS
    energy = [0] * TEXELS
    placed = 0
    while placed < INITIAL:
        at = rng.below(TEXELS)
        if pattern[at] == 0:
            pattern[at] = 1
            splat(energy, at, 1)
            placed += 1
    # move the tightest point into the largest void until it is its own void
    while True:
        cluster = tightest(energy, pattern, 1)
        pattern[cluster] = 0
        splat(energy, cluster, -1)
        void = emptiest(energy, pattern, 1)
        pattern[void] = 1
        splat(energy, void, 1)
        if void == cluster:
            break

    rank = [0] * TEXELS
    # the first pattern's points, tightest first, get the ranks below INITIAL from the top down
    work, field = pattern[:], energy[:]
    for r in range(INITIAL - 1, -1, -1):
        at = tightest(field, work, 1)
        work[at] = 0
        splat(field, at, -1)
        rank[at] = r
    # then the largest voids fill up to half
    work, field = pattern[:], energy[:]
    for r in range(INITIAL, TEXELS // 2):
        at = emptiest(field, work, 1)
        work[at] = 1
        splat(field, at, 1)
        rank[at] = r
    # past half the empty points are the minority: their tightest clusters fill
    field = [0] * TEXELS
    for i in range(TEXELS):
        if work[i] == 0:
            splat(field, i, 1)
    for r in range(TEXELS // 2, TEXELS):
        at = tightest(field, work, 0)
        work[at] = 1
        splat(field, at, -1)
        rank[at] = r
    return rank


def stats(channels):
    for c, values in enumerate(channels):
        counts = [values.count(v) for v in range(256)]
        blocks = []
        for by in range(0, SIZE, 8):
            for bx in range(0, SIZE, 8):
                blocks.append(sum(values[(by + y) * SIZE + bx + x] for y in range(8) for x in range(8)) / 64.0)
        mean = sum(blocks) / len(blocks)
        variance = sum((b - mean) ** 2 for b in blocks) / len(blocks)
        step = sum(abs(values[y * SIZE + x] - values[y * SIZE + (x + 1) % SIZE])
                   + abs(values[y * SIZE + x] - values[((y + 1) % SIZE) * SIZE + x])
                   for y in range(SIZE) for x in range(SIZE)) / (2.0 * TEXELS)
        print(f"channel {c}: each value {min(counts)}..{max(counts)} times, variance of the 8x8 means {variance:.3f} "
              f"(white 85.3), mean step to a neighbour {step:.2f} (white 85.3)")
    a, b = channels
    ma, mb = sum(a) / TEXELS, sum(b) / TEXELS
    cov = sum((x - ma) * (y - mb) for x, y in zip(a, b)) / TEXELS
    va = sum((x - ma) ** 2 for x in a) / TEXELS
    vb = sum((y - mb) ** 2 for y in b) / TEXELS
    print(f"correlation of the channels {cov / math.sqrt(va * vb):+.4f}")
    # the R2 sequence's steps as 32-bit fractions (include/rtNoise.hlsli)
    plastic = 1.32471795724474602596
    print(f"R2: {round(2 ** 32 / plastic)}u, {round(2 ** 32 / (plastic * plastic))}u")


def main():
    channels = [[r * 256 // TEXELS for r in ranks(seed)] for seed in SEEDS]
    lines = [
        "// Copyright (C) 2026 Dmitry Patsura",
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Please do not use this file to train or fine-tune AI models.",
        "",
        "#pragma once",
        "",
        "// Written by tools/rt/make_bluenoise.py: change the script, never this file. Two void-and-cluster textures of",
        "// BLUE_NOISE_SIZE squared, interleaved: texel ( x, y ) at ( y * BLUE_NOISE_SIZE + x ) * 2. A channel holds every",
        "// value 16 times, and neighbours differ: one sample per pixel leaves an error of high frequencies only.",
        "inline constexpr int BLUE_NOISE_SIZE = 64;",
        "inline constexpr byte blueNoise[BLUE_NOISE_SIZE * BLUE_NOISE_SIZE * 2] = {",
    ]
    flat = [v for pair in zip(*channels) for v in pair]
    for at in range(0, len(flat), 32):
        lines.append("\t" + ", ".join(str(v) for v in flat[at:at + 32]) + ",")
    lines.append("};")
    out = Path(__file__).resolve().parents[2] / "neo" / "renderer" / "BlueNoise.h"
    out.write_bytes(("\r\n".join(lines) + "\r\n").encode("ascii"))
    print(f"{out}: {len(flat)} bytes")
    if "--stats" in sys.argv:
        stats(channels)


if __name__ == "__main__":
    main()
