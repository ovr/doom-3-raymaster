#!/usr/bin/env python3
# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.
"""Writes base/maps/rt/gallery.map, the test map of the ray tracing work (demo 20_rt_gallery).

Six rooms joined by doorways with visportals, every one a closed shell of its own, the shells of
two neighbours touching face to face:
  1 materials   floor tiles of 15 retail materials and a grate over a pit, six primitives on
                pedestals (box, wedge, cylinder, cone, bevelled box, sphere), glass, a mirror, one light
  2 colour      red, green and blue projected spots, a light orbiting on a rotating arm
  3 lights      16 small coloured point lights with glowing markers over a metal floor
  4 dark        no light but a faint fill, glowing lamp models and strips (the demo's flashlight)
  5 motion      pulsing and strobing lights, an ambient light, fog, two animated monsters, a fan
  6 hall        a glossy floor 4000 units long, lit along its length

Only retail materials, models and entity defs. The output is the same bytes on every run.

usage: make_gallery.py [output]   (default: base/maps/rt/gallery.map of this repo)
"""

import colorsys
import math
import sys
from pathlib import Path

T = 16          # wall thickness
DOOR_H = 160    # doorway height

# units per texture repeat, as the retail maps use them
SCALE = {
    'textures/base_floor/diafloor': 64, 'textures/base_floor/ghotile3': 64, 'textures/base_floor/stetile4': 64,
    'textures/base_floor/sfloor': 64, 'textures/hell/qfloor': 128, 'textures/caves/sbricks2': 128,
    'textures/base_floor/sflgratetrans1': 32, 'textures/base_floor/squaretile': 32,
    'textures/base_floor/reactorfloor1': (96, 192), 'textures/base_wall/lfwall13f3': 128,
    'textures/base_floor/ghoceiling1': 64, 'textures/base_trim/stesilverswatch': 32,
    'textures/base_trim/gotsilvercol1': (64, 128), 'textures/base_trim/gotrustcol1': (64, 128),
    'textures/caves/metalswatch1': 64, 'textures/mcity/mcityc13': (32, 128), 'textures/hell/cbrick2': 128,
    'textures/base_light/whiteblock': 32, 'textures/base_light/striplight3': (32, 16),
    'textures/base_floor/sofloor1': 128, 'textures/base_floor/sflgrate2': 64,
    'textures/base_wall/stelabwafer2': (64, 32), 'textures/enpro/enwall8a3': (64, 128),
    'textures/base_floor/gothallpanel2': (128, 32), 'textures/base_wall/lfwall5': 256,
    'textures/rock/sharprock': 128, 'textures/base_floor/floorvent02': 64, 'textures/base_floor/stectile1quad2': 64,
    'textures/base_floor/a_bluetex1a_02': 64, 'textures/base_trim/stesilvertrim': 32,
    'textures/base_wall/lfwall27a': 128, 'textures/base_floor/a_diafloor_1b_fin': 64,
}
NODRAW = 'textures/common/nodraw'
VISPORTAL = 'textures/editor/visportal'


def num(v, digits=6):
    if abs(v - round(v)) < 1e-7:
        return str(int(round(v)))
    text = f'{v:.{digits}f}'.rstrip('0').rstrip('.')
    return '0' if text in ('-0', '') else text


def vec(v):
    return ' '.join(num(c) for c in v)


def scale_of(mat):
    s = SCALE.get(mat, 128)
    return s if isinstance(s, tuple) else (s, s)


def normalize(v):
    length = math.sqrt(sum(c * c for c in v))
    return tuple(c / length for c in v)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


class Brush:
    """A convex brush: planes of an outward normal and a point on them."""

    def __init__(self):
        self.sides = []

    def plane(self, normal, point, mat):
        n = normalize(normal)
        self.sides.append((n, -dot(n, point), mat))
        return self

    def text(self, origin=(0, 0, 0)):
        # brushDef3 planes are relative to the entity origin
        lines = ['brushDef3', '{']
        for n, d, mat in self.sides:
            d += dot(n, origin)
            sx, sy = scale_of(mat)
            lines.append(f'( {vec(n)} {num(d)} ) ( ( {num(1 / sx, 8)} 0 0 ) ( 0 {num(1 / sy, 8)} 0 ) ) "{mat}" 0 0 0')
        lines.append('}')
        return lines


def box(lo, hi, mat, sides=None):
    """sides overrides the material of '+x', '-x', '+y', '-y', '+z', '-z'."""
    sides = sides or {}
    b = Brush()
    for axis, name in enumerate('xyz'):
        n = [0, 0, 0]
        n[axis] = 1
        b.plane(tuple(n), hi, sides.get('+' + name, mat))
        n[axis] = -1
        b.plane(tuple(n), lo, sides.get('-' + name, mat))
    return b


def wedge(lo, hi, mat):
    """Full height at -x, sloping down to the floor at +x."""
    b = Brush()
    b.plane((-1, 0, 0), lo, mat).plane((0, 1, 0), hi, mat).plane((0, -1, 0), lo, mat).plane((0, 0, -1), lo, mat)
    b.plane((hi[2] - lo[2], 0, hi[0] - lo[0]), (hi[0], lo[1], lo[2]), mat)
    return b


def bevel_box(lo, hi, c, mat):
    """A box with its vertical and top edges chamfered by c."""
    b = box(lo, hi, mat)
    for sx in (-1, 1):
        for sy in (-1, 1):
            corner = (hi[0] if sx > 0 else lo[0], hi[1] if sy > 0 else lo[1], lo[2])
            b.plane((sx, sy, 0), (corner[0] - sx * c, corner[1], 0), mat)
    for axis in (0, 1):
        for s in (-1, 1):
            n = [0, 0, 1]
            n[axis] = s
            p = [0, 0, hi[2]]
            p[axis] = (hi[axis] if s > 0 else lo[axis]) - s * c
            b.plane(tuple(n), tuple(p), mat)
    return b


class Patch:
    def __init__(self, mat, columns):
        self.mat = mat
        self.columns = columns  # [width][height] of (x, y, z, s, t)

    def text(self):
        w, h = len(self.columns), len(self.columns[0])
        lines = ['patchDef2', '{', f'"{self.mat}"', f'( {w} {h} 0 0 0 )', '(']
        for col in self.columns:
            lines.append('( ' + ' '.join(f'( {vec(p)} )' for p in col) + ' )')
        lines += [')', '}']
        return lines


def revolve(mat, center, profile, s_repeat=2.0, t_repeat=1.0):
    """A surface of revolution about +z: 9 control columns around (quadratic arcs through the
    corners of the square), one row per profile point (radius, height), outward facing."""
    cx, cy, cz = center
    cols = []
    for k in range(9):
        a = math.radians(45 * k)
        f = 1.0 if k % 2 == 0 else math.sqrt(2.0)
        col = []
        for m, (r, z) in enumerate(profile):
            col.append((cx + r * f * math.cos(a), cy + r * f * math.sin(a), cz + z,
                        s_repeat * k / 8, t_repeat * m / (len(profile) - 1)))
        cols.append(col)
    return Patch(mat, cols)


def cylinder(mat, center, r, h, s_repeat=2.0):
    return [revolve(mat, center, [(r, 0), (r, h / 2), (r, h)], s_repeat),
            revolve(mat, center, [(r, h), (r / 2, h), (0, h)], s_repeat)]


def cone(mat, center, r, h):
    return [revolve(mat, center, [(r, 0), (r / 2, h / 2), (0, h)])]


def sphere(mat, center, r):
    return [revolve(mat, center, [(0, -r), (r, -r), (r, 0), (r, r), (0, r)], 2.0)]


class Map:
    def __init__(self):
        self.world = []
        self.entities = []
        self.names = set()

    def add(self, classname, name, keys=(), prims=(), origin=None):
        assert name not in self.names, name
        self.names.add(name)
        kv = [('classname', classname), ('name', name)]
        if origin is not None:
            kv.append(('origin', vec(origin)))
        if prims:
            kv.append(('model', name))
        kv += list(keys)
        self.entities.append((kv, list(prims), origin or (0, 0, 0)))

    def light(self, name, origin, radius=None, color=(1, 1, 1), keys=()):
        kv = [('_color', vec(color))]
        if radius is not None:
            kv.append(('light_radius', vec(radius)))
        self.add('light', name, kv + list(keys), origin=origin)

    def spot(self, name, origin, target, up, right, color, texture='lights/spot01', keys=()):
        kv = [('light_target', vec(target)), ('light_up', vec(up)), ('light_right', vec(right)),
              ('texture', texture), ('_color', vec(color))]
        self.add('light', name, kv + list(keys), origin=origin)

    def text(self, header):
        out = list(header) + ['Version 2', '// entity 0', '{', '"classname" "worldspawn"']
        out += prim_lines(self.world, (0, 0, 0))
        out.append('}')
        for i, (kv, prims, origin) in enumerate(self.entities, 1):
            out += [f'// entity {i}', '{'] + [f'"{k}" "{v}"' for k, v in kv]
            out += prim_lines(prims, origin)
            out.append('}')
        return out


def prim_lines(prims, origin):
    out = []
    for i, p in enumerate(prims):
        out += [f'// primitive {i}', '{']
        out += p.text(origin) if isinstance(p, Brush) else p.text()
        out.append('}')
    return out


class Room:
    """An interior box [x0, x1] x [y0, y1] x [0, h] and its closed shell."""

    def __init__(self, name, x0, y0, x1, y1, h, floor, wall, ceiling):
        self.name, self.x0, self.y0, self.x1, self.y1, self.h = name, x0, y0, x1, y1, h
        self.floor, self.wall, self.ceiling = floor, wall, ceiling
        self.doors = {'-x': [], '+x': [], '-y': [], '+y': []}

    @property
    def center(self):
        return ((self.x0 + self.x1) / 2, (self.y0 + self.y1) / 2)

    def shell(self, floor=True):
        x0, y0, x1, y1, h = self.x0, self.y0, self.x1, self.y1, self.h
        out = []
        if floor:
            out.append(box((x0 - T, y0 - T, -T), (x1 + T, y1 + T, 0), self.floor))
        out.append(box((x0 - T, y0 - T, h), (x1 + T, y1 + T, h + T), self.ceiling))
        # the x walls take the corners
        out += wall_pieces(0, (x0 - T, x0), (y0 - T, y1 + T), h, self.doors['-x'], self.wall)
        out += wall_pieces(0, (x1, x1 + T), (y0 - T, y1 + T), h, self.doors['+x'], self.wall)
        out += wall_pieces(1, (y0 - T, y0), (x0, x1), h, self.doors['-y'], self.wall)
        out += wall_pieces(1, (y1, y1 + T), (x0, x1), h, self.doors['+y'], self.wall)
        return out


def wall_pieces(axis, across, along, h, doors, mat):
    """A wall slab across [a0, a1] on axis, along [b0, b1] on the other one, with doorway holes."""
    def piece(a, b, z):
        lo, hi = [0, 0, z[0]], [0, 0, z[1]]
        lo[axis], hi[axis] = a[0], a[1]
        lo[1 - axis], hi[1 - axis] = b[0], b[1]
        return box(tuple(lo), tuple(hi), mat)

    out = []
    start = along[0]
    for d0, d1 in sorted(doors):
        if d0 > start:
            out.append(piece(across, (start, d0), (0, h)))
        out.append(piece(across, (d0, d1), (DOOR_H, h)))
        start = d1
    if along[1] > start:
        out.append(piece(across, (start, along[1]), (0, h)))
    return out


def connect(a, b, axis, d0, d1):
    """A doorway from room a to room b, which lies on the + side of a along axis."""
    if axis == 0:
        assert b.x0 == a.x1 + 2 * T
        a.doors['+x'].append((d0, d1))
        b.doors['-x'].append((d0, d1))
        mid = a.x1 + T
        lo, hi = (mid - 4, d0, 0), (mid + 4, d1, DOOR_H)
        portal = box(lo, hi, NODRAW, {'+x': VISPORTAL})
    else:
        assert b.y0 == a.y1 + 2 * T
        a.doors['+y'].append((d0, d1))
        b.doors['-y'].append((d0, d1))
        mid = a.y1 + T
        lo, hi = (d0, mid - 4, 0), (d1, mid + 4, DOOR_H)
        portal = box(lo, hi, NODRAW, {'+y': VISPORTAL})
    return portal


def hsv(h, s=0.85, v=1.0):
    return tuple(round(c, 3) for c in colorsys.hsv_to_rgb(h, s, v))


def build():
    m = Map()
    r1 = Room('materials', 0, 0, 768, 768, 256, None, 'textures/base_wall/lfwall13f3', 'textures/base_floor/ghoceiling1')
    r2 = Room('colour', 800, 64, 1440, 704, 256, 'textures/base_floor/ghotile3', 'textures/base_wall/stelabwafer2',
              'textures/base_floor/ghoceiling1')
    r3 = Room('lights', 1472, 64, 2112, 704, 224, 'textures/base_floor/diafloor', 'textures/enpro/enwall8a3',
              'textures/base_floor/ghoceiling1')
    r4 = Room('dark', 1472, 736, 2112, 1376, 256, 'textures/base_floor/sflgrate2', 'textures/base_wall/lfwall27a',
              'textures/base_floor/ghoceiling1')
    r5 = Room('motion', 800, 736, 1440, 1376, 288, 'textures/hell/qfloor', 'textures/hell/cbrick2', 'textures/hell/cbrick2')
    r6 = Room('hall', -2800, 1408, 1200, 1792, 320, 'textures/base_floor/stetile4', 'textures/base_wall/lfwall5',
              'textures/base_floor/ghoceiling1')
    portals = [connect(r1, r2, 0, 320, 448), connect(r2, r3, 0, 320, 448), connect(r3, r4, 1, 1728, 1856),
               connect(r5, r4, 0, 992, 1120), connect(r5, r6, 1, 1056, 1184)]
    w = m.world
    w += r1.shell(floor=False)
    for r in (r2, r3, r4, r5, r6):
        w += r.shell()
    w += portals

    # room 1: a 4 x 4 floor of tiles, under the walls too; the grate covers a pit 64 deep
    grid = [-T, 192, 384, 576, 768 + T]
    tiles = [
        ['textures/base_floor/diafloor', 'textures/base_floor/ghotile3', 'textures/hell/qfloor', 'textures/base_floor/sfloor'],
        ['textures/base_floor/stetile4', 'textures/caves/sbricks2', 'textures/base_floor/sflgratetrans1', 'textures/base_floor/squaretile'],
        ['textures/base_floor/reactorfloor1', 'textures/rock/sharprock', 'textures/base_floor/sofloor1', 'textures/base_floor/a_bluetex1a_02'],
        ['textures/base_floor/floorvent02', 'textures/base_floor/gothallpanel2', 'textures/base_floor/stectile1quad2', 'textures/base_floor/sflgrate2'],
    ]
    for j in range(4):
        for i in range(4):
            lo, hi = (grid[i], grid[j]), (grid[i + 1], grid[j + 1])
            mat = tiles[j][i]
            if mat == 'textures/base_floor/sflgratetrans1':
                w.append(box((lo[0], lo[1], -80), (hi[0], hi[1], -64), 'textures/base_floor/sflgrate2'))
                w.append(box((lo[0], lo[1], -4), (hi[0], hi[1], 0), mat))
            else:
                w.append(box((lo[0], lo[1], -80), (hi[0], hi[1], 0), mat))

    # six pedestals along the north wall, a primitive on each
    ped = 'textures/base_wall/stelabwafer2'
    top = 40
    y = 656
    xs = [112, 224, 336, 448, 560, 672]
    for x in xs:
        w.append(box((x - 32, y - 32, 0), (x + 32, y + 32, top), ped, {'+z': 'textures/base_trim/stesilverswatch'}))
    w.append(box((xs[0] - 20, y - 20, top), (xs[0] + 20, y + 20, top + 40), 'textures/base_trim/stesilverswatch'))
    w.append(wedge((xs[1] - 24, y - 24, top), (xs[1] + 24, y + 24, top + 48), 'textures/hell/cbrick2'))
    w += cylinder('textures/base_trim/gotrustcol1', (xs[2], y, top), 20, 56)
    w += cone('textures/caves/metalswatch1', (xs[3], y, top), 24, 56)
    w.append(bevel_box((xs[4] - 22, y - 22, top), (xs[4] + 22, y + 22, top + 44), 8, 'textures/mcity/mcityc13'))
    w += sphere('textures/base_trim/gotsilvercol1', (xs[5], y, top + 24), 24)

    # a mirror behind the pedestals, glass on a stand in front of the right half
    trim = 'textures/base_trim/stesilvertrim'
    w.append(box((256, 760, 56), (512, 768, 216), trim, {'-y': 'textures/washroom/mirror'}))
    w.append(box((440, 392, 0), (712, 408, 8), trim))
    w.append(box((448, 398, 8), (704, 402, 168), 'textures/glass/glass2'))
    m.add('info_player_start', 'info_player_start_1', [('angle', '90')], origin=(384, 120, 8))
    m.light('r1_light', (384, 384, 224), (600, 600, 320), (1, 1, 1))

    # room 2: three projected spots that overlap on the floor and the pillar
    cx, cy = r2.center
    w += cylinder('textures/base_trim/gotsilvercol1', (cx, cy, 0), 24, 150, 3.0)
    w.append(box((cx + 160, cy - 160, 0), (cx + 224, cy - 96, 64), 'textures/base_trim/stesilverswatch'))
    w += sphere('textures/base_trim/gotsilvercol1', (cx - 176, cy + 140, 32), 32)
    for name, dx, dy, color in (('red', -80, -60, (1, 0.1, 0.05)), ('green', 80, -60, (0.1, 1, 0.1)), ('blue', 0, 90, (0.1, 0.25, 1))):
        m.spot(f'r2_spot_{name}', (cx + dx, cy + dy, 240), (dx * 1.5, dy * 1.5, -240), (0, 210, 0), (210, 0, 0), color)
    m.light('r2_fill', (cx, cy, 200), (380, 380, 260), (0.2, 0.2, 0.2))
    # an arm turning under the ceiling carries a cage lamp and its light
    m.add('func_rotating', 'r2_rotator', [('speed', '40'), ('nopush', '1')],
          [box((cx, cy - 4, 196), (cx + 200, cy + 4, 204), 'textures/base_trim/stesilverswatch'),
           box((cx - 12, cy - 12, 196), (cx + 12, cy + 12, 256), 'textures/base_trim/stesilverswatch')],
          origin=(cx, cy, 200))
    m.add('func_static', 'r2_orbit_lamp', [('model', 'models/mapobjects/lights/cagelight/cagelight.lwo'), ('noshadows', '1'),
                                           ('bind', 'r2_rotator'), ('_color', '1 0.85 0.6')], origin=(cx + 192, cy, 180))
    m.light('r2_orbit_light', (cx + 192, cy, 172), (260, 260, 200), (1, 0.85, 0.6), [('bind', 'r2_rotator')])

    # room 3: 16 small lights with glowing markers, alternately low and high
    sx, sy = r3.x0 + 80, r3.y0 + 80
    for j in range(4):
        for i in range(4):
            k = j * 4 + i
            color = hsv(((k * 5) % 16) / 16)
            p = (sx + 160 * i, sy + 160 * j, 40 if (i + j) % 2 == 0 else 96)
            m.light(f'r3_light_{k + 1}', p, (150, 150, 150), color, [('texture', 'lights/biground1')])
            m.add('func_static', f'r3_bulb_{k + 1}', [('noshadows', '1'), ('_color', vec(color))],
                  [box((p[0] - 4, p[1] - 4, p[2] - 4), (p[0] + 4, p[1] + 4, p[2] + 4), 'textures/base_light/whiteblock')],
                  origin=p)
    cx, cy = r3.center
    w += sphere('textures/base_trim/gotsilvercol1', (cx, cy, 48), 48)
    w += cylinder('textures/base_trim/gotrustcol1', (cx - 240, cy + 160, 0), 20, 128)
    w.append(bevel_box((cx + 200, cy - 200, 0), (cx + 260, cy - 140, 60), 10, 'textures/base_trim/stesilverswatch'))

    # room 4: dark, only glowing fixtures and a faint fill
    cx, cy = r4.center
    strip = 'textures/base_light/striplight3'
    w.append(box((r4.x0 + 160, r4.y1 - 8, 200), (r4.x0 + 288, r4.y1, 216), 'textures/base_trim/stesilvertrim', {'-y': strip}))
    for name, x, color in (('cyan', r4.x0 + 352, (0.2, 0.8, 1)), ('red', r4.x0 + 480, (1, 0.15, 0.1))):
        m.add('func_static', f'r4_strip_{name}', [('_color', vec(color)), ('noshadows', '1')],
              [box((x, r4.y1 - 8, 200), (x + 96, r4.y1, 216), 'textures/base_trim/stesilvertrim', {'-y': strip})],
              origin=(x + 48, r4.y1 - 4, 208))
    m.add('func_static', 'r4_strip_east', [('_color', '1 0.75 0.3'), ('noshadows', '1')],
          [box((r4.x1 - 8, cy - 64, 180), (r4.x1, cy + 64, 196), 'textures/base_trim/stesilvertrim', {'-x': strip})],
          origin=(r4.x1 - 4, cy, 188))
    m.add('func_static', 'r4_cage_1', [('model', 'models/mapobjects/lights/cagelight/cagelight.lwo'), ('_color', '1 0.7 0.16'),
                                       ('rotation', '0 0 -1 -1 0 0 0 1 0')], origin=(r4.x0 + 8, cy - 96, 176))
    m.add('func_static', 'r4_cage_2', [('model', 'models/mapobjects/lights/cagelight/cagelight.lwo'), ('_color', '0.6 0.8 1'),
                                       ('rotation', '0 0 -1 -1 0 0 0 1 0')], origin=(r4.x0 + 8, cy + 96, 176))
    m.add('func_static', 'r4_emergency', [('model', 'models/mapobjects/lights/emerlight1/emerlight1.lwo'), ('_color', '1 0 0')],
          origin=(cx, r4.y0 + 16, 200))
    m.light('r4_fill', (cx, cy, 200), (400, 400, 260), (0.03, 0.035, 0.06))
    w += sphere('textures/base_trim/gotsilvercol1', (cx - 64, cy + 32, 40), 40)
    w += cylinder('textures/caves/metalswatch1', (cx + 120, cy + 140, 0), 28, 200)
    w.append(box((cx + 60, cy - 180, 0), (cx + 156, cy - 84, 96), 'textures/base_trim/stesilverswatch'))
    w.append(box((cx + 84, cy - 156, 96), (cx + 132, cy - 108, 144), 'textures/base_trim/stesilverswatch'))

    # room 5: pulsing and strobing lights, an ambient light, low fog by the north wall, monsters, a fan
    cx, cy = r5.center
    m.light('r5_pulse', (r5.x0 + 112, r5.y0 + 112, 112), (260, 260, 200), (1, 0.35, 0.08), [('texture', 'lights/round_sin')])
    m.light('r5_strobe', (r5.x1 - 112, r5.y0 + 112, 112), (260, 260, 200), (0.25, 0.45, 1), [('texture', 'lights/round_strobe')])
    m.light('r5_ambient', (cx, cy, 144), (320, 320, 144), (0.1, 0.09, 0.08), [('texture', 'lights/ambientLight'), ('noshadows', '1')])
    m.light('r5_key', (cx + 96, cy + 64, 260), (420, 420, 300), (0.85, 0.75, 0.6))
    # ground fog: its box ends inside the walls and under the floor, where its edges are hidden
    m.light('r5_fog', (cx, cy, 71), (330, 330, 79), (0.45, 0.42, 0.4),
            [('texture', 'fogs/basicFog'), ('shaderParm3', '900'), ('noshadows', '1'), ('nospecular', '1'), ('nodiffuse', '1')])
    m.add('func_animate', 'r5_hellknight', [('model', 'monster_demon_hellknight'), ('start_anim', 'idle'), ('angle', '0')],
          origin=(cx - 120, r5.y1 - 160, 0))
    m.add('func_animate', 'r5_imp', [('model', 'monster_demon_imp'), ('start_anim', 'idle'), ('angle', '0')],
          origin=(cx - 40, r5.y0 + 200, 0))
    m.add('func_rotating', 'r5_fan', [('model', 'models/mapobjects/erebus/erebus4_fan_lo.lwo'), ('speed', '90'), ('nopush', '1')],
          origin=(cx - 120, cy - 60, r5.h - 40))
    w.append(box((cx + 120, cy - 240, 0), (cx + 200, cy - 160, 80), 'textures/base_trim/stesilverswatch'))

    # room 6: the long hall, columns along both walls, a light every 400 units
    for k, x in enumerate(range(r6.x0 + 200, r6.x1, 400)):
        tint = (1, 0.92, 0.8) if k % 2 == 0 else (0.8, 0.9, 1)
        m.light(f'r6_light_{k + 1}', (x, (r6.y0 + r6.y1) / 2, 290), (360, 360, 420), tint)
    for x in range(r6.x0 + 400, r6.x1, 400):
        for y0, y1 in ((r6.y0, r6.y0 + 32), (r6.y1 - 32, r6.y1)):
            w.append(box((x - 24, y0, 0), (x + 24, y1, r6.h), 'textures/base_trim/gotsilvercol1'))
    w += sphere('textures/base_trim/gotsilvercol1', (r6.x0 + 300, (r6.y0 + r6.y1) / 2, 64), 64)
    w += sphere('textures/base_trim/gotsilvercol1', (r6.x0 + 2200, r6.y0 + 96, 40), 40)
    w.append(box((r6.x0 + 1200, r6.y1 - 140, 0), (r6.x0 + 1296, r6.y1 - 44, 96), 'textures/base_trim/stesilverswatch'))
    return m


HEADER = [
    '// Copyright (C) 2026 Dmitry Patsura',
    '// SPDX-License-Identifier: GPL-3.0-or-later',
    '// Please do not use this file to train or fine-tune AI models.',
    '// written by tools/rt/make_gallery.py: the ray tracing test rooms of demo 20_rt_gallery',
]


def main():
    repo = Path(__file__).resolve().parents[2]
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else repo / 'base' / 'maps' / 'rt' / 'gallery.map'
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, 'w', encoding='ascii', newline='\r\n') as f:
        f.write('\n'.join(build().text(HEADER)) + '\n')
    print(f'wrote {out}')


if __name__ == '__main__':
    main()
