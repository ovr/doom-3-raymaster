#!/usr/bin/env python3
# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.
"""Writes base/maps/rt/lightlab.map, the test map of the lighting work (demos 27-30).

Closed rooms that share no wall and no doorway, so no room's light or bounce reaches another; a
demo moves between them with setviewpos. The surfaces are the materials of make_lab_textures.py,
whose albedo, specular and emission are known numbers; one retail monster.

  bays      four equal bays under lights of radius 0.1, 4, 12 and 32 units (light_size): poles 12,
            80 and 160 units before the back wall, a grate, a table, a sphere
  stations  an animated hellknight, a rotor under a lamp, a pole between a steady and a strobing
            light, a caged lamp on a wall, a table that casts no shadow onto itself and a pole that
            casts none, a door to a lit annex whose light reaches through the wall, and a
            mirror on that wall: it reflects the floor the annex's light must not reach
  bounce    a red and a green wall, one spot onto the floor, two boxes, an alcove
  emissive  no light: a ceiling panel, a row of small coloured panels, a tiny bright one, strips
            along the floor
  gloss     floor strips from mirror to rough with a glowing bar, a sphere and a pole each, a flat
            and a rippled mirror, three coloured lights

The output is the same bytes on every run.

usage: make_lightlab.py [output]   (default: base/maps/rt/lightlab.map of this repo)
"""

import math
import sys
from pathlib import Path

import mapgen
from mapgen import NODRAW, T, Map, Room, box, connect, hsv, num, turned_box, vec

RT = 'textures/rt/'
WHITE, GREY, DARK = RT + 'white', RT + 'grey', RT + 'dark'
RED, GREEN = RT + 'red', RT + 'green'
CHECKER, GRATE, EMIT = RT + 'checker', RT + 'grate', RT + 'emit'
CHROME, RIPPLE, GOLD = RT + 'chrome', RT + 'chrome_ripple', RT + 'gold'
ROUGH = [RT + f'rough{p:02d}' for p in (0, 10, 20, 35, 50, 70)]

# units per texture repeat: 32 unit squares, 16 unit grate cells
mapgen.SCALE.update({CHECKER: 64, GRATE: 64, RIPPLE: 64})

BAY_SIZES = (0.1, 4, 12, 32)


def sphere(mat, center, r):
    """32 steps around: a reflection shows every facet."""
    return mapgen.sphere(mat, center, r, (8, 8))


def pole(x, y, h, mat=WHITE, half=4):
    return box((x - half, y - half, 0), (x + half, y + half, h), mat)


def table(x0, y0, x1, y1, h, mat):
    """A top 4 thick on four legs."""
    out = [box((x0, y0, h - 4), (x1, y1, h), mat)]
    for x in (x0 + 2, x1 - 6):
        for y in (y0 + 2, y1 - 6):
            out.append(box((x, y, 0), (x + 4, y + 4, h - 4), mat))
    return out


def lamp(m, name, lo, hi, color):
    """A glowing brush, an entity of its own: its emission is the entity's colour."""
    origin = tuple((a + b) / 2 for a, b in zip(lo, hi))
    m.add('func_static', name, [('_color', vec(color)), ('noshadows', '1')], [box(lo, hi, EMIT)], origin=origin)


def bays(m):
    r = Room('bays', 0, 0, 1024, 512, 256, CHECKER, WHITE, WHITE)
    w = m.world
    w += r.shell()
    for x in (256, 512, 768):
        w.append(box((x - 8, 192, 0), (x + 8, 512, 256), WHITE))
    for k, size in enumerate(BAY_SIZES):
        bx = 256 * k
        m.light(f'bay{k + 1}_light', (bx + 128, 224, 208), (240, 448, 400), (1, 1, 1), [('light_size', num(size))])
        # the poles' shadows land side by side on the back wall
        w.append(pole(bx + 48, 496, 160))
        w.append(pole(bx + 96, 432, 160))
        w.append(pole(bx + 140, 352, 160))
        w.append(box((bx + 176, 451, 0), (bx + 224, 453, 112), NODRAW, {'-y': GRATE}))
        w += table(bx + 24, 272, bx + 88, 320, 64, WHITE)
        w += sphere(WHITE, (bx + 200, 300, 20), 20)
    m.add('info_player_start', 'info_player_start_1', [('angle', '90')], origin=(128, 56, 8))


def stations(m):
    r = Room('stations', 0, 704, 1280, 1344, 256, CHECKER, WHITE, WHITE)
    annex = Room('annex', 1312, 896, 1568, 1152, 256, CHECKER, WHITE, WHITE)
    portal = connect(r, annex, 0, 960, 1088)
    w = m.world
    w += r.shell() + annex.shell() + [portal]

    m.add('func_animate', 'st_hellknight', [('model', 'monster_demon_hellknight'), ('start_anim', 'idle'), ('angle', '270')],
          origin=(160, 1216, 0))
    m.light('st_knight_light', (240, 1100, 208), (240, 400, 480), (1, 0.97, 0.92))

    # two blades, one above the other where they cross
    m.add('func_rotating', 'st_rotor', [('speed', '60'), ('nopush', '1')],
          [box((392, 1126, 96), (568, 1146, 100), GREY), box((470, 1048, 100), (490, 1224, 104), GREY)], origin=(480, 1136, 100))
    m.light('st_rotor_light', (480, 1136, 220), (220, 220, 480), (0.9, 0.95, 1))

    w.append(pole(800, 1216, 160, half=6))
    m.light('st_steady', (720, 1088, 176), (240, 240, 400), (1, 0.8, 0.55))
    m.light('st_strobe', (880, 1088, 176), (240, 240, 400), (0.4, 0.6, 1), [('texture', 'lights/round_strobe')])

    # a lamp on the north wall behind seven bars between two rings
    cx, cy, cz = 1120, 1316, 150
    cage = []
    for z in (cz - 34, cz + 32):
        cage += [box((cx - 24, cy - 24, z), (cx + 24, cy - 22, z + 2), DARK), box((cx - 24, cy - 22, z), (cx - 22, r.y1, z + 2), DARK),
                 box((cx + 22, cy - 22, z), (cx + 24, r.y1, z + 2), DARK)]
    for degrees in range(0, 181, 30):
        x = cx + round(22 * math.cos(math.radians(degrees)))
        y = cy - round(22 * math.sin(math.radians(degrees)))
        cage.append(box((x - 1, y - 1, cz - 32), (x + 1, y + 1, cz + 32), DARK))
    m.add('func_static', 'st_cage', [], cage, origin=(cx, cy, cz))
    m.light('st_cage_light', (cx, cy, cz), (300, 300, 320), (1, 0.75, 0.45))

    w += table(240, 744, 320, 800, 64, WHITE)
    w += table(400, 744, 480, 800, 64, RT + 'white_noselfshadow')
    w.append(pole(560, 772, 128))
    w.append(pole(608, 772, 128, RT + 'white_noshadows'))
    m.light('st_south_light', (424, 880, 208), (300, 220, 480), (1, 1, 1))

    # the door slides up into the wall; the annex light's volume reaches 320 units into the room
    m.add('func_door', 'st_door', [('movedir', '-1'), ('lip', '8'), ('speed', '80'), ('no_touch', '1'), ('toggle', '1')],
          [box((1290, 960, 0), (1302, 1088, 160), DARK)], origin=(1296, 1024, 80))
    m.light('annex_light', (1440, 1024, 200), (480, 320, 300), (1, 0.8, 0.5))
    w.append(box((1276, 816, 16), (1280, 944, 176), CHROME))


def bounce(m):
    r = Room('bounce', 1408, 0, 1920, 512, 320, WHITE, WHITE, WHITE, {'-x': RED, '+x': GREEN})
    cx, cy = (r.x0 + r.x1) // 2, (r.y0 + r.y1) // 2
    alcove = Room('alcove', cx - 96, 544, cx + 96, 672, 192, WHITE, WHITE, WHITE)
    portal = connect(r, alcove, 1, cx - 64, cx + 64)
    w = m.world
    w += r.shell() + alcove.shell() + [portal]
    w.append(turned_box((cx - 110, cy + 40), (40, 40), (0, 160), 20, WHITE))
    w.append(turned_box((cx + 90, cy - 40), (40, 40), (0, 80), -15, WHITE))
    # a disc of 300 units on the floor: no wall is lit directly. The target lies under the floor, clear of the falloff's end
    m.spot('bounce_spot', (cx, cy, 304), (0, 0, -400), (0, 197, 0), (197, 0, 0), (1, 1, 1))


def emissive(m):
    r = Room('emissive', 2112, 0, 2880, 512, 256, CHECKER, GREY, GREY)
    w = m.world
    w += r.shell()
    x0 = r.x0
    lamp(m, 'em_panel', (x0 + 96, 160, 252), (x0 + 288, 352, 256), (0.7, 0.66, 0.6))
    for i in range(5):
        x = x0 + 368 + 56 * i
        lamp(m, f'em_small_{i + 1}', (x, 508, 112), (x + 32, 512, 144), hsv(i / 5))
    lamp(m, 'em_tiny', (r.x1 - 4, 252, 124), (r.x1, 260, 132), (1, 1, 1))
    lamp(m, 'em_strip_south', (x0 + 64, 0, 4), (x0 + 704, 4, 12), (0.2, 0.8, 1))
    lamp(m, 'em_strip_west', (x0, 96, 4), (x0 + 4, 416, 12), (1, 0.55, 0.15))
    w.append(box((x0 + 160, 224, 0), (x0 + 224, 288, 64), WHITE))
    w += sphere(WHITE, (x0 + 480, 400, 32), 32)
    for i in range(3):
        w.append(pole(x0 + 620 + 40 * i, 200 + 60 * i, 128))
    w += table(x0 + 640, 380, x0 + 720, 440, 64, WHITE)


def gloss(m):
    r = Room('gloss', 2112, 832, 3072, 1344, 256, None, WHITE, WHITE, {'+y': CHECKER})
    w = m.world
    w += r.shell(floor=False)
    x0, y0, y1 = r.x0, r.y0, r.y1
    for i, mat in enumerate(ROUGH):
        # the strips run under the walls
        lo = x0 + 160 * i - (T if i == 0 else 0)
        hi = x0 + 160 * (i + 1) + (T if i == len(ROUGH) - 1 else 0)
        w.append(box((lo, y0 - T, -T), (hi, y1 + T, 0), mat))
        xc = x0 + 160 * i + 80
        lamp(m, f'gl_bar_{i + 1}', (xc - 8, y1 - 4, 16), (xc + 8, y1, 176), hsv(i / 6))
        w += sphere(CHROME if i % 2 == 0 else GOLD, (xc + 24, y1 - 128, 32), 32)
        w.append(pole(xc - 40, y1 - 208, 128))
    w.append(box((x0, y0 + 128, 32), (x0 + 4, y0 + 384, 192), DARK, {'+x': CHROME}))
    w.append(box((r.x1 - 4, y0 + 128, 32), (r.x1, y0 + 384, 192), DARK, {'-x': RIPPLE}))
    for name, x, color in (('warm', x0 + 160, (1, 0.85, 0.7)), ('white', x0 + 480, (1, 1, 1)), ('cool', x0 + 800, (0.7, 0.85, 1))):
        m.light(f'gl_light_{name}', (x, y0 + 208, 224), (420, 420, 300), color)


def build():
    m = Map()
    for room in (bays, stations, bounce, emissive, gloss):
        room(m)
    return m


def main():
    repo = Path(__file__).resolve().parents[2]
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else repo / 'base' / 'maps' / 'rt' / 'lightlab.map'
    mapgen.write_map(out, build(), mapgen.header('make_lightlab.py', 'the light rooms of demos 27-30'))


if __name__ == '__main__':
    main()
