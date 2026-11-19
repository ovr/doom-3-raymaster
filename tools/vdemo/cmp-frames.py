# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.
"""Compares the PNG frames of two visual demo runs byte for byte.

usage: python cmp-frames.py <baseline run dir> <new run dir> [demo ...]

Every demo dir of the baseline that holds PNG frames is compared (or only the demos named). A frame
is bad when the new run lacks it or its bytes differ; a PNG the baseline does not have is extra.
Exit code 0 when every frame exists and is identical and there was at least one, 1 otherwise.
"""
import filecmp
import sys
from pathlib import Path


def pngs(directory: Path) -> set[str]:
    return {p.name for p in directory.glob('*.png')} if directory.is_dir() else set()


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    base, new = Path(sys.argv[1]), Path(sys.argv[2])
    only = set(sys.argv[3:])
    demos = sorted(d.name for d in base.iterdir() if d.is_dir() and pngs(d))
    if only:
        unknown = only - set(demos)
        if unknown:
            print(f'not in the baseline: {", ".join(sorted(unknown))}', file=sys.stderr)
            return 2
        demos = [d for d in demos if d in only]
    bad = total = 0
    for demo in demos:
        names = sorted(pngs(base / demo))
        found = pngs(new / demo)
        missing = [n for n in names if n not in found]
        differ = [n for n in names if n in found and not filecmp.cmp(base / demo / n, new / demo / n, shallow=False)]
        extra = sorted(found - set(names))
        first = min(missing + differ + extra, default='')
        total += len(names)
        bad += len(missing) + len(differ) + len(extra)
        print(f'{demo:22} {len(names):5} frames | differ {len(differ)} | missing {len(missing)} | extra {len(extra)}'
              + (f' | first: {first}' if first else ''))
    ok = bad == 0 and total > 0
    print(f'TOTAL {total} frames in {len(demos)} demos: ' + ('IDENTICAL' if ok else 'DIFFERENT'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
