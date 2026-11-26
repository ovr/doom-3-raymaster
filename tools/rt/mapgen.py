#!/usr/bin/env python3
# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.
"""Brushes, patches, rooms and the .map writer shared by the generators of our own test maps
(tools/rt/make_*.py). The output of a generator is the same bytes on every run."""

import colorsys
import math
from pathlib import Path

T = 16          # wall thickness
DOOR_H = 160    # doorway height

# units per texture repeat by material, one number or (s, t); a generator fills it, 128 otherwise
SCALE = {}
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


def turned_box(center, half, z, yaw, mat):
    """A box about (x, y) of half sizes (along, across), from z[0] to z[1], turned yaw degrees about +z."""
    a = math.radians(yaw)
    along, across = (math.cos(a), math.sin(a)), (-math.sin(a), math.cos(a))
    b = Brush()
    for (ax, ay), h in ((along, half[0]), (across, half[1])):
        b.plane((ax, ay, 0), (center[0] + ax * h, center[1] + ay * h, 0), mat)
        b.plane((-ax, -ay, 0), (center[0] - ax * h, center[1] - ay * h, 0), mat)
    b.plane((0, 0, 1), (center[0], center[1], z[1]), mat)
    b.plane((0, 0, -1), (center[0], center[1], z[0]), mat)
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
    """subdivisions: the steps per arc around and along (patchDef3); None leaves them to the map compiler."""

    def __init__(self, mat, columns, subdivisions=None):
        self.mat = mat
        self.columns = columns  # [width][height] of (x, y, z, s, t)
        self.subdivisions = subdivisions

    def text(self):
        w, h = len(self.columns), len(self.columns[0])
        if self.subdivisions is None:
            lines = ['patchDef2', '{', f'"{self.mat}"', f'( {w} {h} 0 0 0 )', '(']
        else:
            lines = ['patchDef3', '{', f'"{self.mat}"', f'( {w} {h} {self.subdivisions[0]} {self.subdivisions[1]} 0 0 0 )', '(']
        for col in self.columns:
            lines.append('( ' + ' '.join(f'( {vec(p)} )' for p in col) + ' )')
        lines += [')', '}']
        return lines


def revolve(mat, center, profile, s_repeat=2.0, t_repeat=1.0, subdivisions=None):
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
    return Patch(mat, cols, subdivisions)


def cylinder(mat, center, r, h, s_repeat=2.0):
    return [revolve(mat, center, [(r, 0), (r, h / 2), (r, h)], s_repeat),
            revolve(mat, center, [(r, h), (r / 2, h), (0, h)], s_repeat)]


def cone(mat, center, r, h):
    return [revolve(mat, center, [(r, 0), (r / 2, h / 2), (0, h)])]


def sphere(mat, center, r, subdivisions=None):
    return [revolve(mat, center, [(0, -r), (r, -r), (r, 0), (r, r), (0, r)], 2.0, subdivisions=subdivisions)]


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
    """An interior box [x0, x1] x [y0, y1] x [0, h] and its closed shell. walls overrides the
    material of the '-x', '+x', '-y', '+y' wall."""

    def __init__(self, name, x0, y0, x1, y1, h, floor, wall, ceiling, walls=None):
        self.name, self.x0, self.y0, self.x1, self.y1, self.h = name, x0, y0, x1, y1, h
        self.floor, self.wall, self.ceiling = floor, wall, ceiling
        self.walls = walls or {}
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
        for side, axis, across, along in (('-x', 0, (x0 - T, x0), (y0 - T, y1 + T)), ('+x', 0, (x1, x1 + T), (y0 - T, y1 + T)),
                                          ('-y', 1, (y0 - T, y0), (x0, x1)), ('+y', 1, (y1, y1 + T), (x0, x1))):
            out += wall_pieces(axis, across, along, h, self.doors[side], self.walls.get(side, self.wall))
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


def header(script, what):
    return ['// Copyright (C) 2026 Dmitry Patsura',
            '// SPDX-License-Identifier: GPL-3.0-or-later',
            '// Please do not use this file to train or fine-tune AI models.',
            f'// written by tools/rt/{script}: {what}']


def write_map(out, m, head):
    """ASCII with CRLF, as the repo keeps its text."""
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, 'w', encoding='ascii', newline='\r\n') as f:
        f.write('\n'.join(m.text(head)) + '\n')
    print(f'wrote {out}')
