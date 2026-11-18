# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the gamma ramp of the Direct3D 12 present blit (design check G8), frame by frame.

usage: python check-gamma.py <scene run> <back buffer run> [demo ...] [--gamma G] [--brightness B]

Both runs play the same demos with the same r_gamma and r_brightness; the second one also has
r_d3d12CaptureBackBuffer 1, so its frames are what the window shows after the blit. Every frame of
the second run must be the frame of the first through the ramp R_SetColorMappings builds
(neo/renderer/RenderSystem_init.cpp), channel by channel, within 1: the ramp is rebuilt here in the
same float arithmetic, and the blit rounds each 16-bit value to the byte round(v * 255 / 65535).

Exit code 0 when at least one frame was compared and none is off by more than 1, 1 otherwise.
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def color_mappings(gamma: float, brightness: float) -> np.ndarray:
    """The 256 bytes the blit shows for each input byte."""
    inv_gamma = np.float32(1.0) / np.float32(gamma)
    j = np.float32(0.0)
    shown = np.empty(256, dtype=np.int64)
    for i in range(256):
        value = np.float32(0xFFFF) * np.power(j / np.float32(255.0), inv_gamma) + np.float32(0.5)
        ramp = min(max(int(np.floor(value)), 0), 0xFFFF)
        shown[i] = (ramp * 255 + 32767) // 65535
        j = np.float32(j + np.float32(brightness))
    return shown


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('scene')
    parser.add_argument('back')
    parser.add_argument('demos', nargs='*')
    parser.add_argument('--gamma', type=float, default=1.0)
    parser.add_argument('--brightness', type=float, default=1.0)
    args = parser.parse_args()

    scene_root, back_root = Path(args.scene), Path(args.back)
    demos = args.demos or sorted(p.name for p in scene_root.iterdir() if p.is_dir() and any(p.glob('*.png')))
    lut = color_mappings(args.gamma, args.brightness)
    print(f'r_gamma {args.gamma}, r_brightness {args.brightness}: 0 -> {lut[0]}, 64 -> {lut[64]}, 128 -> {lut[128]}, '
          f'192 -> {lut[192]}, 255 -> {lut[255]}')
    frames = failed = missing = 0
    worst = 0
    off_by_one = 0
    for demo in demos:
        demo_frames = demo_failed = 0
        for scene_png in sorted((scene_root / demo).glob('*.png')):
            back_png = back_root / demo / scene_png.name
            if not back_png.exists():
                missing += 1
                continue
            scene = np.asarray(Image.open(scene_png).convert('RGB')).astype(np.int64)
            back = np.asarray(Image.open(back_png).convert('RGB')).astype(np.int64)
            if scene.shape != back.shape:
                print(f'  {demo}/{scene_png.name}: {back.shape} against {scene.shape}')
                demo_failed += 1
                demo_frames += 1
                continue
            delta = np.abs(back - lut[scene])
            frame_worst = int(delta.max())
            worst = max(worst, frame_worst)
            off_by_one += int((delta == 1).sum())
            demo_frames += 1
            if frame_worst > 1:
                demo_failed += 1
                row, col, channel = (int(v) for v in np.argwhere(delta > 1)[0])
                print(f'  {demo}/{scene_png.name}: max delta {frame_worst}, {int((delta > 1).sum())} channels over 1, '
                      f'first at png ({col}, {row}) channel {channel}: scene {scene[row, col, channel]} -> '
                      f'{back[row, col, channel]}, expected {lut[scene[row, col, channel]]}')
        print(f'{demo:24} {demo_frames:5} frames | over 1: {demo_failed}')
        frames += demo_frames
        failed += demo_failed
    verdict = 'PASS' if frames > 0 and failed == 0 and missing == 0 else 'FAIL'
    print(f'TOTAL {frames} frames in {len(demos)} demos: max delta {worst}, {off_by_one} channels off by one, '
          f'{failed} frames over 1, {missing} missing: {verdict}')
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
