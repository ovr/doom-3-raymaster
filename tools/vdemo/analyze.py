#!/usr/bin/env python3
# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.
"""Analyse the frames of a visual demo run (tools/vdemo/run-vdemos.ps1, the vdemo command).

A run dir holds one dir per demo with frames t<tic>_s<step>.png (and vdemo.json). Frames with the
same name in two runs show the same game tic and virtual-clock step, so they must match. A frame is
"on a tic" when its render alpha is 0 (the "frameTags" of vdemo.json), else when its step is 0.

  analyze.py RUN                         smoothness of RUN alone
  analyze.py RUN --baseline BASE         + SSIM/PSNR and the difference metrics of the frames RUN
                                         and BASE share, heat maps
  analyze.py RUN --baseline BASE --bars tools/vdemo/bars-d3d12.json [--layer msaa4]
                                         + the per-demo bars of another API against BASE
  analyze.py RUN --sheets DIR --sheet-prefix 10-   contact sheets DIR/10-<demo>-sheet.png
  analyze.py RUN --video                 an mp4 per demo (ffmpeg on PATH)
  analyze.py RUN --pair 04_lighting:04b_lighting_zfail   two passes of a scene in one run
  analyze.py RUN --baseline BASE --offset-tics 1   RUN draws tic T's pose at alpha 0 after tic T+1
  analyze.py RUN --baseline BASE --hold  frames between tics against the baseline frame of their tic

Writes report.md and report.json into --out (default RUN/analysis). Exit code 1 when a frame on a
tic falls below --ssim-threshold against the baseline (with --bars: a demo misses one of its bars),
a baseline frame is missing, or a demo has more pops than its baseline; 2 on bad arguments.

The difference of two frames is the largest per-channel |delta| of each pixel. Per frame: SSIM of
the luma, max delta, the fractions of pixels with delta > 2 and > 8, and the blob: the pixel count of
the largest 4-connected region of delta > 8 left after a 3x3 erosion, which removes every line and
speckle up to 2 px thick. Compiler noise and rasterization lines stay out of the blob; a missing
surface, a wrong blend or a flipped cull leaves a solid one, so the blob is the main bug detector.

Bars (a JSON file, "demos": {demo: {ssimMin, ssimMean, frac8, blob}}, "layers": {layer: {"demos":
{...}}} for runs with other cvars on both APIs) hold for the worst frame of a demo: min SSIM >=
ssimMin, mean SSIM >= ssimMean, every frac8 <= frac8, every blob <= blob; the repeats and pops of the
demo must equal the baseline's.

Smoothness looks at frames that are neighbours in virtual time (one capture interval apart): the
mean absolute luma difference between them, repeats (no pixel changed) and pops (a difference
above --pop-factor times the median of the demo, and above --pop-floor).
"""

import argparse
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

FRAME_RE = re.compile(r"^t(\d+)_s(\d+)\.png$")


@dataclass
class Frame:
    tic: int
    step: int
    alpha: float | None
    path: Path

    @property
    def on_tic(self):
        return self.alpha == 0 if self.alpha is not None else self.step == 0


def load_rgb(path):
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.uint8)


def luma(rgb):
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def local_mean(x, window):
    """Mean over every window x window block, valid region only (integral image)."""
    ii = np.pad(x, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    s = ii[window:, window:] - ii[:-window, window:] - ii[window:, :-window] + ii[:-window, :-window]
    return s / (window * window)


def ssim(a, b, window=7):
    """Mean SSIM of two luma images (Wang et al. 2004, uniform window as in scikit-image)."""
    x = a.astype(np.float64)
    y = b.astype(np.float64)
    c1 = (0.01 * 255) ** 2
    c2 = (0.03 * 255) ** 2
    n = window * window
    unbiased = n / (n - 1)
    mx = local_mean(x, window)
    my = local_mean(y, window)
    vx = (local_mean(x * x, window) - mx * mx) * unbiased
    vy = (local_mean(y * y, window) - my * my) * unbiased
    cxy = (local_mean(x * y, window) - mx * my) * unbiased
    num = (2 * mx * my + c1) * (2 * cxy + c2)
    den = (mx * mx + my * my + c1) * (vx + vy + c2)
    return float(np.mean(num / den))


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    if mse == 0:
        return math.inf
    return 10 * math.log10(255 * 255 / mse)


# inferno-like ramp for the heat maps
HEAT_STOPS = np.array([
    [0, 0, 4], [40, 11, 84], [101, 21, 110], [159, 42, 99],
    [212, 72, 66], [245, 125, 21], [250, 193, 39], [252, 255, 164]], dtype=np.float64)


def heat_colors(values):
    """values in 0..1 -> RGB uint8"""
    t = np.clip(values, 0, 1) * (len(HEAT_STOPS) - 1)
    i = np.minimum(t.astype(np.int64), len(HEAT_STOPS) - 2)
    f = (t - i)[..., None]
    return (HEAT_STOPS[i] * (1 - f) + HEAT_STOPS[i + 1] * f).astype(np.uint8)


def diff_region(a, b):
    """Pixels that differ at all: count and bounding box (x0, y0, x1, y1), or None."""
    mask = np.any(a != b, axis=2)
    count = int(mask.sum())
    if count == 0:
        return 0, None
    ys, xs = np.nonzero(mask)
    return count, [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())]


def erode3x3(mask):
    """A pixel stays when its whole 3x3 neighbourhood is set; outside the frame counts as unset."""
    h, w = mask.shape
    padded = np.pad(mask, 1, constant_values=False)
    out = np.ones_like(mask)
    for dy in range(3):
        for dx in range(3):
            out &= padded[dy:dy + h, dx:dx + w]
    return out


def largest_component(mask):
    """Pixel count and bounding box (x0, y0, x1, y1) of the largest 4-connected region of mask.

    Runs of set pixels per row are joined with union-find wherever runs of neighbouring rows share a
    column, so the cost follows the number of runs, not of pixels."""
    h = mask.shape[0]
    edges = np.diff(np.pad(mask.astype(np.int8), ((0, 0), (1, 1))), axis=1)
    run_y, run_start = np.nonzero(edges == 1)
    _, run_end = np.nonzero(edges == -1)
    count = len(run_start)
    if count == 0:
        return 0, None
    parent = list(range(count))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    row_first = np.searchsorted(run_y, np.arange(h + 1))
    for y in range(1, h):
        i, i_end = row_first[y - 1], row_first[y]
        j, j_end = row_first[y], row_first[y + 1]
        while i < i_end and j < j_end:
            if run_start[i] < run_end[j] and run_start[j] < run_end[i]:
                parent[find(i)] = find(j)
            if run_end[i] < run_end[j]:
                i += 1
            else:
                j += 1
    sizes = {}
    for k in range(count):
        root = find(k)
        sizes[root] = sizes.get(root, 0) + int(run_end[k] - run_start[k])
    best = max(sizes, key=sizes.get)
    members = [k for k in range(count) if find(k) == best]
    box = [int(min(run_start[k] for k in members)), int(min(run_y[k] for k in members)),
           int(max(run_end[k] for k in members)) - 1, int(max(run_y[k] for k in members))]
    return sizes[best], box


def difference_metrics(a, b):
    """max delta, the fractions of delta > 2 and > 8, and the eroded delta > 8 blob of two RGB frames."""
    delta = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2)
    big = delta > 8
    blob, blob_box = largest_component(erode3x3(big))
    return {"maxDelta": int(delta.max()), "frac2": float(np.mean(delta > 2)), "frac8": float(np.mean(big)),
            "blob": blob, "blobBox": blob_box}


NO_DIFFERENCE = {"maxDelta": 0, "frac2": 0.0, "frac8": 0.0, "blob": 0, "blobBox": None}


def load_bars(path, layer):
    """The bars of every demo, the layer's entries replacing the plain ones."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    bars = dict(data.get("demos", {}))
    if layer:
        layers = data.get("layers", {})
        if layer not in layers:
            raise ValueError(f"no layer '{layer}' in {path}")
        bars.update(layers[layer].get("demos", {}))
    return bars


def check_bars(demo, entry, bar):
    """The bars a demo misses: (metric, value, bar, frame) rows."""
    c = entry.get("compare")
    if not c or not c["frames"]:
        return [("frames", 0, "> 0", "-")]
    if bar is None:
        return [("bar", "none", "an entry in the bars file", "-")]
    rows = c["frames"]
    misses = []
    worst_ssim = min(rows, key=lambda r: r["ssim"])
    if worst_ssim["ssim"] < bar["ssimMin"]:
        misses.append(("min SSIM", round(worst_ssim["ssim"], 5), f">= {bar['ssimMin']}", worst_ssim["frame"]))
    if c["meanSsim"] < bar["ssimMean"]:
        misses.append(("mean SSIM", round(c["meanSsim"], 5), f">= {bar['ssimMean']}", "-"))
    worst_frac = max(rows, key=lambda r: r["frac8"])
    if worst_frac["frac8"] > bar["frac8"]:
        misses.append(("delta>8 fraction", round(worst_frac["frac8"], 5), f"<= {bar['frac8']}", worst_frac["frame"]))
    worst_blob = max(rows, key=lambda r: r["blob"])
    if worst_blob["blob"] > bar["blob"]:
        misses.append(("blob px", worst_blob["blob"], f"<= {bar['blob']}", worst_blob["frame"]))
    smooth, base_smooth = entry["smoothness"], entry.get("baselineSmoothness", {})
    for key, label in (("repeats", "repeats"), ("pops", "pops")):
        value = smooth.get(key, 0)
        base_value = base_smooth.get(key, 0)
        if key == "pops":
            value, base_value = len(value or []), len(base_value or [])
        if value != base_value:
            misses.append((label, value, f"= {base_value} (baseline)", "-"))
    return misses


def write_heatmap(path, base, run, label):
    """baseline | run | |difference| x8 as a heat map, half size each."""
    diff = np.abs(base.astype(np.int16) - run.astype(np.int16)).max(axis=2) / 255.0
    heat = heat_colors(np.sqrt(np.clip(diff * 8, 0, 1)))
    h, w = base.shape[:2]
    panels = [Image.fromarray(p).resize((w // 2, h // 2), Image.BILINEAR) for p in (base, run, heat)]
    out = Image.new("RGB", (3 * (w // 2), h // 2 + 18), (16, 16, 16))
    for k, p in enumerate(panels):
        out.paste(p, (k * (w // 2), 18))
    draw = ImageDraw.Draw(out)
    draw.text((6, 3), f"{label}   baseline | run | difference (x8, sqrt)", fill=(230, 230, 230))
    out.save(path)


def list_frames(demo_dir):
    alphas = {}
    info = demo_dir / "vdemo.json"
    if info.exists():
        try:
            alphas = {f["file"]: f.get("alpha") for f in json.loads(info.read_text()).get("frameTags", [])}
        except (ValueError, OSError, KeyError, TypeError):
            pass
    frames = {}
    for p in demo_dir.iterdir():
        m = FRAME_RE.match(p.name)
        if m:
            frames[p.name] = Frame(int(m.group(1)), int(m.group(2)), alphas.get(p.name), p)
    return frames


def demo_steps(demo_dir):
    info = demo_dir / "vdemo.json"
    if info.exists():
        try:
            return int(json.loads(info.read_text()).get("stepsPerTic", 4)), json.loads(info.read_text())
        except (ValueError, OSError):
            pass
    return 4, {}


def smoothness(frames, steps, pop_factor, pop_floor):
    """Differences between frames that are neighbours in virtual time."""
    ordered = sorted(frames.values(), key=lambda f: f.tic * steps + f.step)
    if len(ordered) < 2:
        return {"pairs": 0}
    times = [f.tic * steps + f.step for f in ordered]
    interval = min(b - a for a, b in zip(times, times[1:]))
    pairs = []
    previous = None
    for f, t in zip(ordered, times):
        image = luma(load_rgb(f.path))
        if previous is not None and t - previous[0] == interval:
            d = float(np.mean(np.abs(image - previous[1])))
            pairs.append({"from": previous[2], "to": f.path.name, "diff": d})
        previous = (t, image, f.path.name)
    if not pairs:
        return {"pairs": 0, "interval": interval}
    diffs = np.array([p["diff"] for p in pairs])
    median = float(np.median(diffs))
    limit = max(pop_floor, pop_factor * median)
    pops = [p for p in pairs if p["diff"] > limit]
    repeats = [p for p in pairs if p["diff"] == 0.0]
    return {
        "pairs": len(pairs),
        "interval": interval,
        "meanDiff": float(diffs.mean()),
        "medianDiff": median,
        "maxDiff": float(diffs.max()),
        "repeats": len(repeats),
        "pops": [{"from": p["from"], "to": p["to"], "diff": round(p["diff"], 3)} for p in pops],
        "popLimit": limit,
    }


def contact_sheet(frames, steps, path, title, columns=6, max_frames=24, thumb_width=320):
    ordered = sorted(frames.values(), key=lambda f: f.tic * steps + f.step)
    on_tic = [f for f in ordered if f.on_tic] or ordered
    if len(on_tic) > max_frames:
        picks = [on_tic[round(i * (len(on_tic) - 1) / (max_frames - 1))] for i in range(max_frames)]
    else:
        picks = on_tic
    if not picks:
        return
    first = Image.open(picks[0].path)
    thumb_height = round(first.height * thumb_width / first.width)
    rows = math.ceil(len(picks) / columns)
    label_h = 16
    sheet = Image.new("RGB", (columns * thumb_width, 24 + rows * (thumb_height + label_h)), (12, 12, 12))
    draw = ImageDraw.Draw(sheet)
    draw.text((8, 6), title, fill=(235, 235, 235))
    for k, f in enumerate(picks):
        x = (k % columns) * thumb_width
        y = 24 + (k // columns) * (thumb_height + label_h)
        with Image.open(f.path) as image:
            sheet.paste(image.convert("RGB").resize((thumb_width, thumb_height), Image.BILINEAR), (x, y + label_h))
        alpha = f"  alpha {f.alpha:g}" if f.alpha is not None else ""
        draw.text((x + 4, y + 2), f"tic {f.tic}  step {f.step}{alpha}", fill=(200, 200, 200))
    sheet.save(path)


def make_video(frames, steps, path, fps):
    ordered = sorted(frames.values(), key=lambda f: f.tic * steps + f.step)
    if not ordered or shutil.which("ffmpeg") is None:
        return False
    # ffmpeg reads relative paths of the list against the list's folder, a temp one
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as listing:
        for f in ordered:
            listing.write(f"file '{f.path.resolve().as_posix()}'\nduration {1.0 / fps:.6f}\n")
        listing.write(f"file '{ordered[-1].path.resolve().as_posix()}'\n")
        list_path = listing.name
    try:
        result = subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", list_path,
             "-c:v", "libx264", "-crf", "18", "-pix_fmt", "yuv420p", "-r", str(fps), str(path)],
            check=False)
        return result.returncode == 0
    finally:
        Path(list_path).unlink(missing_ok=True)


def match_frames(run_frames, base_frames, only_tics, offset_tics, hold):
    """(run name, baseline name) pairs and the baseline frames without a partner.

    The run frame of tic T matches the baseline frame of tic T - offset_tics, same step; with hold
    every run frame of a tic matches the baseline frame on that tic."""
    base_on_tic = {f.tic: n for n, f in base_frames.items() if f.on_tic}
    base_by_key = {(f.tic, f.step): n for n, f in base_frames.items()}
    pairs = []
    for name, f in sorted(run_frames.items()):
        if only_tics and not f.on_tic:
            continue
        partner = base_on_tic.get(f.tic - offset_tics) if hold else base_by_key.get((f.tic - offset_tics, f.step))
        if partner is not None:
            pairs.append((name, partner))
    matched = {b for _, b in pairs}
    missing = sorted(n for n, f in base_frames.items() if n not in matched and (not only_tics or f.on_tic))
    return pairs, missing


def compare(run_frames, base_frames, ssim_threshold, heat_dir, heat_count, demo, only_tics, offset_tics=0, hold=False):
    pairs, missing = match_frames(run_frames, base_frames, only_tics, offset_tics, hold)
    rows = []
    for name, base_name in pairs:
        a = load_rgb(base_frames[base_name].path)
        b = load_rgb(run_frames[name].path)
        row = {"frame": name, "baseline": base_name}
        if a.shape != b.shape:
            rows.append(row | {"ssim": 0.0, "psnr": 0.0, "identical": False, "diffPixels": -1, "bbox": None,
                               "maxDelta": 255, "frac2": 1.0, "frac8": 1.0, "blob": a.shape[0] * a.shape[1],
                               "blobBox": None})
            continue
        count, bbox = diff_region(a, b)
        if count == 0:
            rows.append(row | {"ssim": 1.0, "psnr": math.inf, "identical": True, "diffPixels": 0, "bbox": None}
                        | NO_DIFFERENCE)
            continue
        rows.append(row | {"ssim": ssim(luma(a), luma(b)), "psnr": psnr(a, b), "identical": False,
                           "diffPixels": count, "bbox": bbox} | difference_metrics(a, b))
    # the frames with the biggest blob first: those are the likely bugs
    worst = sorted((r for r in rows if not r["identical"]), key=lambda r: (-r["blob"], r["ssim"]))[:heat_count]
    for r in worst:
        name = r["frame"]
        label = name if r["baseline"] == name else f"{name} vs baseline {r['baseline']}"
        write_heatmap(heat_dir / f"{demo}-{Path(name).stem}-heat.png", load_rgb(base_frames[r["baseline"]].path),
                      load_rgb(run_frames[name].path),
                      f"{demo} {label}  SSIM {r['ssim']:.5f}  PSNR {r['psnr']:.2f} dB  {r['diffPixels']} px differ  "
                      f"d>8 {100 * r['frac8']:.3f}%  blob {r['blob']} px at {r['blobBox']}")
        r["heatmap"] = f"{demo}-{Path(name).stem}-heat.png"
    below = [r for r in rows if r["ssim"] < ssim_threshold]
    return {
        "compared": len(rows),
        "identical": sum(r["identical"] for r in rows),
        "minSsim": min((r["ssim"] for r in rows), default=None),
        "meanSsim": float(np.mean([r["ssim"] for r in rows])) if rows else None,
        "minPsnr": min((r["psnr"] for r in rows), default=None),
        "maxDelta": max((r["maxDelta"] for r in rows), default=None),
        "maxFrac2": max((r["frac2"] for r in rows), default=None),
        "maxFrac8": max((r["frac8"] for r in rows), default=None),
        "maxBlob": max((r["blob"] for r in rows), default=None),
        "belowThreshold": [r["frame"] for r in below],
        "missing": missing,
        "frames": rows,
    }


def luma_rows(run_frames, base_frames):
    # (tic, run luma, baseline luma): the mean display luma of each tic frame both runs have
    rows = []
    for name, frame in sorted(run_frames.items()):
        base = base_frames.get(name)
        if base is not None and frame.on_tic:
            rows.append((frame.tic, float(luma(load_rgb(frame.path)).mean()), float(luma(load_rgb(base.path)).mean())))
    return rows


def luma_report(args, demos, out):
    report, ratios = {}, []
    print(f"{'demo':<20} {'frames':>6} {'run':>7} {'base':>7} {'ratio':>6} {'p10':>6} {'p90':>6}")
    for demo in demos:
        run_dir, base_dir = args.run / demo, args.baseline / demo
        if not run_dir.is_dir() or not base_dir.is_dir():
            continue
        rows = luma_rows(list_frames(run_dir), list_frames(base_dir))
        if not rows:
            continue
        frame_ratios = np.array([r / max(b, 0.5) for _, r, b in rows])
        ratios += list(frame_ratios)
        run_mean, base_mean = np.mean([r for _, r, _ in rows]), np.mean([b for _, _, b in rows])
        p10, p90 = np.percentile(frame_ratios, [10, 90])
        report[demo] = {"frames": len(rows), "run": run_mean, "baseline": base_mean, "ratio": run_mean / max(base_mean, 0.5),
                        "p10": p10, "p90": p90, "tics": [[t, r, b] for t, r, b in rows]}
        print(f"{demo:<20} {len(rows):>6} {run_mean:7.1f} {base_mean:7.1f} {report[demo]['ratio']:6.3f} {p10:6.3f} {p90:6.3f}")
    if ratios:
        p10, p50, p90 = np.percentile(ratios, [10, 50, 90])
        print(f"all frames: median ratio {p50:.3f}, p10 {p10:.3f}, p90 {p90:.3f}")
        report["all"] = {"frames": len(ratios), "median": p50, "p10": p10, "p90": p90}
    (out / "luma.json").write_text(json.dumps(report, indent=1))
    return 0


def fmt(value, digits=4):
    if value is None:
        return "-"
    if isinstance(value, float) and math.isinf(value):
        return "inf"
    return f"{value:.{digits}f}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run", type=Path, help="run dir with one dir of frames per demo")
    parser.add_argument("--baseline", type=Path, help="baseline run dir to compare against")
    parser.add_argument("--out", type=Path, help="report dir (default RUN/analysis)")
    parser.add_argument("--demos", help="comma separated demo names (default: all in RUN)")
    parser.add_argument("--ssim-threshold", type=float, default=0.98,
                        help="a tic frame below this SSIM against the baseline is a regression (0.98)")
    parser.add_argument("--all-steps", action="store_true",
                        help="compare the frames between tics too, not only those on a tic")
    parser.add_argument("--offset-tics", type=int, default=0,
                        help="RUN frames of tic T show the pose of baseline tic T - N (interpolation behind)")
    parser.add_argument("--hold", action="store_true",
                        help="compare every RUN frame with the baseline frame on its tic (a renderer that "
                             "repeats the tic pose between tics); implies --all-steps")
    parser.add_argument("--pop-factor", type=float, default=4.0, help="pop: neighbour diff above this x median")
    parser.add_argument("--pop-floor", type=float, default=6.0, help="pop: and above this mean luma diff")
    parser.add_argument("--pop-tolerance", type=int, default=0, help="extra pops allowed over the baseline")
    parser.add_argument("--heatmaps", type=int, default=3, help="heat maps of the worst frames per demo")
    parser.add_argument("--sheets", type=Path, help="write contact sheets into this dir")
    parser.add_argument("--sheet-prefix", default="", help="file name prefix of the sheets, e.g. 10-")
    parser.add_argument("--video", action="store_true", help="an mp4 per demo into the report dir")
    parser.add_argument("--video-fps", type=int, default=60)
    parser.add_argument("--pair", action="append", default=[], metavar="A:B",
                        help="compare demo B against demo A of the same run (e.g. a second pass with other cvars)")
    parser.add_argument("--bars", type=Path,
                        help="per-demo bars (tools/vdemo/bars-d3d12.json); they replace --ssim-threshold")
    parser.add_argument("--layer", help="take the bars of this layer of the bars file where it has them")
    parser.add_argument("--luma", action="store_true",
                        help="only the mean display luma of the run against the baseline, per demo (luma.json)")
    args = parser.parse_args()
    if args.hold:
        args.all_steps = True

    if not args.run.is_dir():
        print(f"no run dir {args.run}", file=sys.stderr)
        return 2
    if args.baseline is not None and not args.baseline.is_dir():
        print(f"no baseline dir {args.baseline}", file=sys.stderr)
        return 2
    bars = None
    if args.bars is not None:
        if args.baseline is None:
            print("--bars needs --baseline", file=sys.stderr)
            return 2
        try:
            bars = load_bars(args.bars, args.layer)
        except (OSError, ValueError) as error:
            print(f"--bars: {error}", file=sys.stderr)
            return 2
    elif args.layer:
        print("--layer needs --bars", file=sys.stderr)
        return 2
    out = args.out or (args.run / "analysis")
    out.mkdir(parents=True, exist_ok=True)
    if args.sheets:
        args.sheets.mkdir(parents=True, exist_ok=True)

    demos = sorted(d.name for d in args.run.iterdir() if d.is_dir() and list_frames(d))
    if args.baseline is not None:
        demos = sorted(set(demos) | {d.name for d in args.baseline.iterdir() if d.is_dir() and list_frames(d)})
    if args.demos:
        wanted = set(args.demos.split(","))
        demos = [d for d in demos if d in wanted]
    if args.luma:
        if args.baseline is None:
            print("--luma needs --baseline", file=sys.stderr)
            return 2
        return luma_report(args, demos, out)

    report = {"run": str(args.run), "baseline": str(args.baseline) if args.baseline else None,
              "ssimThreshold": args.ssim_threshold, "offsetTics": args.offset_tics, "hold": args.hold,
              "bars": str(args.bars) if args.bars else None, "layer": args.layer, "demos": {}}
    regressions = []
    bar_misses = []
    for demo in demos:
        run_dir = args.run / demo
        run_frames = list_frames(run_dir) if run_dir.is_dir() else {}
        steps, info = demo_steps(run_dir) if run_dir.is_dir() else (4, {})
        entry = {"frames": len(run_frames), "status": info.get("status"), "map": info.get("map")}
        print(f"{demo}: {len(run_frames)} frames", flush=True)
        entry["smoothness"] = smoothness(run_frames, steps, args.pop_factor, args.pop_floor)
        if args.baseline is not None:
            base_dir = args.baseline / demo
            base_frames = list_frames(base_dir) if base_dir.is_dir() else {}
            entry["compare"] = compare(run_frames, base_frames, args.ssim_threshold, out, args.heatmaps, demo,
                                       not args.all_steps, args.offset_tics, args.hold)
            base_steps, _ = demo_steps(base_dir) if base_dir.is_dir() else (4, {})
            base_smooth = smoothness(base_frames, base_steps, args.pop_factor, args.pop_floor)
            entry["baselineSmoothness"] = base_smooth
            entry["baselinePops"] = len(base_smooth.get("pops", []))
            c = entry["compare"]
            if bars is not None:
                misses = check_bars(demo, entry, bars.get(demo))
                entry["barMisses"] = [list(m) for m in misses]
                bar_misses += [(demo,) + m for m in misses]
                if misses:
                    regressions.append(f"{demo}: misses {len(misses)} bars")
            elif c["belowThreshold"]:
                regressions.append(f"{demo}: {len(c['belowThreshold'])} tic frames below SSIM {args.ssim_threshold}")
            if c["missing"]:
                regressions.append(f"{demo}: {len(c['missing'])} baseline frames missing")
            new_pops = len(entry["smoothness"].get("pops", [])) - entry["baselinePops"]
            if new_pops > args.pop_tolerance:
                regressions.append(f"{demo}: {new_pops} more pops than the baseline")
        if info.get("status") not in (None, "ok"):
            regressions.append(f"{demo}: the demo ended '{info.get('status')}'")
        if args.sheets and run_frames:
            contact_sheet(run_frames, steps, args.sheets / f"{args.sheet_prefix}{demo}-sheet.png",
                          f"{demo}  {info.get('map', '')}  ({len(run_frames)} frames)")
        if args.video and run_frames:
            entry["video"] = make_video(run_frames, steps, out / f"{demo}.mp4", args.video_fps)
        report["demos"][demo] = entry

    report["pairs"] = {}
    for pair in args.pair:
        first, _, second = pair.partition(":")
        if not (args.run / first).is_dir() or not (args.run / second).is_dir():
            print(f"--pair {pair}: both demos must be in the run", file=sys.stderr)
            return 2
        c = compare(list_frames(args.run / second), list_frames(args.run / first), args.ssim_threshold, out,
                    args.heatmaps, f"{first}-vs-{second}", not args.all_steps)
        report["pairs"][pair] = c
        if c["belowThreshold"]:
            regressions.append(f"{pair}: {len(c['belowThreshold'])} frames below SSIM {args.ssim_threshold}")

    report["regressions"] = regressions
    (out / "report.json").write_text(json.dumps(report, indent=1, default=lambda v: None))

    lines = [f"# Visual demo report", "", f"Run: `{args.run}`"]
    if args.baseline is not None:
        matching = "every frame against the frame on its tic" if args.hold else \
            "all frames" if args.all_steps else "frames on a tic"
        offset = f", run tic T against baseline tic T-{args.offset_tics}" if args.offset_tics else ""
        lines += [f"Baseline: `{args.baseline}`, SSIM threshold {args.ssim_threshold} on {matching}{offset}"]
    if args.bars is not None:
        lines += [f"Bars: `{args.bars}`" + (f", layer {args.layer}" if args.layer else "")]
    lines += ["", "| demo | frames | compared | identical | min SSIM | mean SSIM | min PSNR | max d | "
                  "max d>2 % | max d>8 % | max blob | missing | mean diff | repeats (base) | pops (base) |",
              "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for demo, e in report["demos"].items():
        c = e.get("compare", {})
        s = e["smoothness"]
        pops = len(s.get("pops", []))
        base_pops = f" ({e['baselinePops']})" if "baselinePops" in e else ""
        base_repeats = f" ({e['baselineSmoothness'].get('repeats', '-')})" if "baselineSmoothness" in e else ""
        percent = lambda key: fmt(100 * c[key], 3) if c.get(key) is not None else "-"
        lines.append(f"| {demo} | {e['frames']} | {c.get('compared', '-')} | {c.get('identical', '-')} | "
                     f"{fmt(c.get('minSsim'))} | {fmt(c.get('meanSsim'))} | {fmt(c.get('minPsnr'), 2)} | "
                     f"{c.get('maxDelta', '-')} | {percent('maxFrac2')} | {percent('maxFrac8')} | "
                     f"{c.get('maxBlob', '-')} | "
                     f"{len(c.get('missing', [])) if c else '-'} | {fmt(s.get('meanDiff'), 2)} | "
                     f"{s.get('repeats', '-')}{base_repeats} | {pops}{base_pops} |")
    if bars is not None:
        lines += ["", "## Bars missed", ""]
        if bar_misses:
            lines += ["| demo | metric | value | bar | worst frame |", "|---|---|---|---|---|"]
            lines += [f"| {d} | {m} | {v} | {b} | {f} |" for d, m, v, b, f in bar_misses]
        else:
            lines.append("none")
    if args.baseline is not None:
        lines += ["", "## Frames that differ from the baseline", ""]
        for demo, e in report["demos"].items():
            differ = [r for r in e["compare"]["frames"] if not r["identical"]]
            if not differ:
                continue
            lines.append(f"- **{demo}**: {len(differ)} frames differ")
            for r in sorted(differ, key=lambda r: (-r["blob"], r["ssim"]))[:5]:
                heat = f", heat map `{r['heatmap']}`" if "heatmap" in r else ""
                versus = f" (vs `{r['baseline']}`)" if r["baseline"] != r["frame"] else ""
                lines.append(f"  - `{r['frame']}`{versus} SSIM {fmt(r['ssim'], 5)}, PSNR {fmt(r['psnr'], 2)} dB, "
                             f"{r['diffPixels']} px in box {r['bbox']}, max d {r['maxDelta']}, "
                             f"d>8 {fmt(100 * r['frac8'], 3)} %, blob {r['blob']} px at {r['blobBox']}{heat}")
    if report["pairs"]:
        lines += ["", "## Pairs (B against A in this run)", "",
                  "| pair | compared | identical | min SSIM | mean SSIM | min PSNR | worst frames |", "|---|---|---|---|---|---|---|"]
        for pair, c in report["pairs"].items():
            worst = ", ".join(f"`{r['frame']}` {fmt(r['ssim'], 5)}" for r in
                              sorted((r for r in c["frames"] if not r["identical"]), key=lambda r: r["ssim"])[:3])
            lines.append(f"| {pair} | {c['compared']} | {c['identical']} | {fmt(c['minSsim'])} | {fmt(c['meanSsim'])} | "
                         f"{fmt(c['minPsnr'], 2)} | {worst or '-'} |")
    lines += ["", "## Pops", ""]
    any_pops = False
    for demo, e in report["demos"].items():
        for p in e["smoothness"].get("pops", []):
            any_pops = True
            lines.append(f"- {demo}: `{p['from']}` -> `{p['to']}` diff {p['diff']}")
    if not any_pops:
        lines.append("none")
    lines += ["", "## Verdict", ""]
    lines += [f"- {r}" for r in regressions] or ["no regressions"]
    (out / "report.md").write_text("\n".join(lines) + "\n")

    print("\n".join(lines[3:]))
    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main())
