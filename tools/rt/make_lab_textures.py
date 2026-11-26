#!/usr/bin/env python3
# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.
"""Writes the surfaces of the light lab (base/maps/rt/lightlab.map): base/textures/rt/*.tga and
base/materials/rt_lab.mtr, materials whose albedo, specular colour and emission are known numbers.

  white.tga         16x16 white: every flat albedo is this times a stage colour. Not the engine's
                    _white: a diffuse map is read as YCoCg, where _white is a little yellow and
                    _black is blue
  checker.tga       64x64, 2x2 squares of two greys
  grate.tga         64x64, 4x4 cells of bars with holes in the alpha (alpha-tested, two-sided)
  ripple_local.tga  64x64 normal map, two periods of a sine in both directions

Colours are written as the engine reads them, sRGB encoded. A gloss material names its roughness in
percent at r_gbufferGloss 1.2: roughness = 1 - sqrt( luminance of the linear specular ) * 1.2.
The output is the same bytes on every run.

usage: make_lab_textures.py [base folder]   (default: base of this repo)
"""

import math
import struct
import sys
from pathlib import Path

GLOSS = 1.2

# flat albedos, sRGB
FLAT = {
    'white': (0.88, 0.88, 0.88),
    'grey': (0.5, 0.5, 0.5),
    'dark': (0.25, 0.25, 0.25),
    'black': (0.04, 0.04, 0.04),
    'red': (0.8, 0.1, 0.08),
    'green': (0.15, 0.7, 0.12),
    'blue': (0.12, 0.2, 0.8),
}
# floor strips: a dark albedo under a grey specular of this roughness
ROUGHNESS = (0.0, 0.1, 0.2, 0.35, 0.5, 0.7)
GLOSS_ALBEDO = 0.1
# metals: no albedo to speak of, the colour is the specular; ( specular sRGB, bump map )
METALS = {
    'chrome': ((1.0, 1.0, 1.0), None),
    'chrome_ripple': ((1.0, 1.0, 1.0), 'textures/rt/ripple_local.tga'),
    'gold': ((1.0, 0.78, 0.34), None),
}
METAL_ALBEDO = 0.02


def srgb(linear):
    return 12.92 * linear if linear <= 0.0031308 else 1.055 * linear ** (1 / 2.4) - 0.055


def specular_of(roughness):
    """The sRGB grey whose G-buffer roughness is this."""
    return srgb(min(1.0, ((1.0 - roughness) / GLOSS) ** 2))


def num(v):
    text = f'{v:.3f}'.rstrip('0').rstrip('.')
    return text or '0'


def write_tga(path, rows):
    """Uncompressed, bottom row first; rows of (r, g, b) or (r, g, b, a) from the top."""
    depth = len(rows[0][0])
    body = bytearray(struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, len(rows[0]), len(rows), depth * 8, 8 if depth == 4 else 0))
    for row in reversed(rows):
        for p in row:
            body += bytes((p[2], p[1], p[0]) + tuple(p[3:]))
    path.write_bytes(bytes(body))
    print(f'wrote {path}')


def white():
    return [[(255, 255, 255)] * 16 for _ in range(16)]


def checker():
    light, dark = (214, 214, 214), (150, 150, 150)
    return [[light if (x // 32 + y // 32) % 2 == 0 else dark for x in range(64)] for y in range(64)]


def grate():
    # 4 pixel bars around 12 pixel holes
    return [[(122, 124, 128, 255 if x % 16 < 4 or y % 16 < 4 else 0) for x in range(64)] for y in range(64)]


def ripple():
    rows = []
    k = 2 * math.pi * 2 / 64
    for y in range(64):
        row = []
        for x in range(64):
            # the slopes of 0.35 / k * sin( k x ) * sin( k y ), sampled at the texel centres
            dx = 0.35 * math.cos(k * (x + 0.5)) * math.sin(k * (y + 0.5))
            dy = 0.35 * math.sin(k * (x + 0.5)) * math.cos(k * (y + 0.5))
            length = math.sqrt(dx * dx + dy * dy + 1)
            row.append(tuple(int(round((c / length * 0.5 + 0.5) * 255)) for c in (-dx, -dy, 1)))
        rows.append(row)
    return rows


def stage(blend, image, color=None, extra=()):
    lines = ['\t{', f'\t\tblend {blend}', f'\t\tmap {image}']
    if color is not None:
        if color[0] == color[1] == color[2]:
            lines.append(f'\t\trgb {num(color[0])}')
        else:
            lines.append(f'\t\tcolor {num(color[0])}, {num(color[1])}, {num(color[2])}, 1')
    lines += [f'\t\t{e}' for e in extra]
    lines.append('\t}')
    return lines


def material(name, body, keys=(), editor='textures/rt/white.tga'):
    return [f'textures/rt/{name}', '{', f'\tqer_editorimage {editor}'] + [f'\t{k}' for k in keys] + body + ['}', '']


def materials():
    out = ['// Copyright (C) 2026 Dmitry Patsura',
           '// SPDX-License-Identifier: GPL-3.0-or-later',
           '// Please do not use this file to train or fine-tune AI models.',
           '// written by tools/rt/make_lab_textures.py: the surfaces of the light lab, see there',
           '']
    white_map = 'textures/rt/white.tga'
    for name, color in FLAT.items():
        out += material(name, stage('diffusemap', white_map, color))
    # shadows: one that casts none, one that casts none onto itself and its like
    out += material('white_noshadows', stage('diffusemap', white_map, FLAT['white']), ['noShadows'])
    out += material('white_noselfshadow', stage('diffusemap', white_map, FLAT['white']), ['noSelfShadow'])
    out += material('checker', ['\tdiffusemap textures/rt/checker.tga'], editor='textures/rt/checker.tga')
    out += material('grate', stage('diffusemap', 'textures/rt/grate.tga', extra=['alphaTest 0.5']), ['twoSided'],
                    editor='textures/rt/grate.tga')
    for roughness in ROUGHNESS:
        s = specular_of(roughness)
        out += material(f'rough{round(roughness * 100):02d}',
                        stage('diffusemap', white_map, (GLOSS_ALBEDO,) * 3) + stage('specularmap', '_white', (s, s, s)))
    for name, (specular, bump) in METALS.items():
        body = [f'\tbumpmap {bump}'] if bump else []
        out += material(name, body + stage('diffusemap', white_map, (METAL_ALBEDO,) * 3) + stage('specularmap', '_white', specular))
    # a lamp: nearly black, plus the entity's colour (white on a world brush)
    out += material('emit', stage('diffusemap', white_map, FLAT['black']) + stage('add', '_white', extra=['colored']))
    return out


def main():
    base = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / 'base'
    textures = base / 'textures' / 'rt'
    textures.mkdir(parents=True, exist_ok=True)
    write_tga(textures / 'white.tga', white())
    write_tga(textures / 'checker.tga', checker())
    write_tga(textures / 'grate.tga', grate())
    write_tga(textures / 'ripple_local.tga', ripple())
    mtr = base / 'materials' / 'rt_lab.mtr'
    mtr.parent.mkdir(parents=True, exist_ok=True)
    with open(mtr, 'w', encoding='ascii', newline='\r\n') as f:
        f.write('\n'.join(materials()))
    print(f'wrote {mtr}')


if __name__ == '__main__':
    main()
