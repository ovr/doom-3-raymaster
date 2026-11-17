# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the frames of a visual demo run made with r_testClearPattern 1, pixel for pixel.

usage: python check-pattern.py <run dir> [demo ...] [--no-masked]

With r_testClearPattern 1 every frame ends with clears that cover it whole (RB_DrawTestClearPattern
in neo/renderer/tr_backend_frame.cpp), so each PNG must equal the pattern rebuilt here: a background
of (32, 64, 96), 64x32 corners red (bottom left), green (bottom right), blue (top left) and white
(top right), one yellow pixel at (100, 50) and a 128x64 centre rectangle whose red only was cleared
to 255. Positions are in GL coordinates, origin at the bottom left; PNG row 0 is the top row.
--no-masked expects no centre rectangle, for a backend without caps.maskedColorClear.

Exit code 0 when every frame matches and there was at least one, 1 otherwise.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

BACKGROUND = (32, 64, 96)


def expected_pattern(width: int, height: int, masked: bool) -> np.ndarray:
    image = np.empty((height, width, 3), dtype=np.uint8)
    image[:, :] = BACKGROUND

    def fill(x: int, y: int, w: int, h: int, color: tuple[int, int, int]) -> None:
        # GL rows y .. y + h - 1 from the bottom are PNG rows height - y - h .. height - y - 1
        image[height - y - h:height - y, x:x + w] = color

    fill(0, 0, 64, 32, (255, 0, 0))
    fill(width - 64, 0, 64, 32, (0, 255, 0))
    fill(0, height - 32, 64, 32, (0, 0, 255))
    fill(width - 64, height - 32, 64, 32, (255, 255, 255))
    fill(100, 50, 1, 1, (255, 255, 0))
    if masked:
        x, y = width // 2 - 64, height // 2 - 32
        image[height - y - 64:height - y, x:x + 128, 0] = 255
    return image


def check_frame(path: Path, masked: bool) -> str:
    frame = np.asarray(Image.open(path).convert('RGB'))
    height, width = frame.shape[:2]
    wrong = np.any(frame != expected_pattern(width, height, masked), axis=2)
    count = int(wrong.sum())
    if count == 0:
        return ''
    row, col = (int(v) for v in np.argwhere(wrong)[0])
    return (f'{count} pixels differ, first at png ({col}, {row}) = gl ({col}, {height - 1 - row}): '
            f'{tuple(int(v) for v in frame[row, col])}')


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    masked = '--no-masked' not in sys.argv[1:]
    if not args:
        print(__doc__, file=sys.stderr)
        return 2
    run = Path(args[0])
    only = set(args[1:])
    demos = sorted(d for d in run.iterdir() if d.is_dir() and any(d.glob('*.png')) and (not only or d.name in only))
    if only - {d.name for d in demos}:
        print(f'no frames for: {", ".join(sorted(only - {d.name for d in demos}))}', file=sys.stderr)
        return 2
    total = bad = 0
    for demo in demos:
        frames = sorted(demo.glob('*.png'))
        failures = [(f.name, message) for f in frames if (message := check_frame(f, masked))]
        total += len(frames)
        bad += len(failures)
        print(f'{demo.name:22} {len(frames):5} frames | wrong {len(failures)}'
              + (f' | first: {failures[0][0]}: {failures[0][1]}' if failures else ''))
    ok = bad == 0 and total > 0
    print(f'TOTAL {total} frames: ' + ('PATTERN OK' if ok else 'PATTERN WRONG'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
