#!/usr/bin/env python3
"""Bake the face scans into normal maps for the watch, one per head angle.

    python3 make_normal_map.py            # writes ../face_normals.h and ../previews/normal-maps*.png

The head is rendered from a grid of yaw (turn) and pitch (nod) angles, cropped to a circle so the
dial ticks fit around it, with small holes in the scan filled from their neighbours. The watch picks
the view matching how it's tilted and lights it with the hour light.

A second set comes from the open-mouth scan (me-mouth.obj), moved onto the first by align_scans.py.
That scan only covers the front, so anything it's missing (the top of the head) is taken from the
first scan; the watch dissolves between the two sets around meal times.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from mesh import load_obj, rotation, save_png, splat  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FACE_DIR = os.path.dirname(HERE)

SIZE = 200
HEAD_RADIUS = 72       # head is clipped to this circle, leaving room for the numbers either side
HEAD_HEIGHT_PX = 140   # crown to chin-ish, before the circle clip
SUPERSAMPLE = 2
NARROWING_FROM_CENTER = 30  # px below the centre (about the cheekbones) where the outline starts narrowing
CHIN_BELOW_TOP = 0.26  # metres from the crown down to the chin; below it only the neck is kept
NECK_HALF_WIDTH = 0.04  # metres either side of the head's centre line

MOUTH_SMOOTHING = 2  # passes of a 3x3 average over the open-mouth scan's normals, whose surface is noisier

YAW_STEPS, YAW_STEP_DEG = 11, 5      # -25..25 degrees, turning left/right
PITCH_STEPS, PITCH_STEP_DEG = 5, 5   # -10..10 degrees, nodding up/down (the scan has no underside)


def head_frame(v):
    """Top of the head and its centre line (x, z), from the first scan."""
    ymax = v[:, 1].max()
    top = v[v[:, 1] > ymax - 0.10]
    return ymax, np.median(top[:, 0]), np.median(top[:, 2])


def head_points(v, n, frame):
    """Just the head and a little neck: everything near the top of the head, within a cylinder,
    and below the chin only a neck-wide strip so bits of shoulder don't poke out."""
    ymax, cx, cz = frame
    keep = (v[:, 1] > ymax - 0.26) & (np.hypot(v[:, 0] - cx, v[:, 2] - cz) < 0.14)
    keep &= (v[:, 1] > ymax - CHIN_BELOW_TOP) | (np.abs(v[:, 0] - cx) < NECK_HALF_WIDTH)
    return v[keep], n[keep]


def fill_holes(normals, mask, iterations=12):
    """Close small gaps in the mask and fill their normals by averaging valid neighbours."""
    closed = mask.copy()
    for _ in range(2):  # dilate
        closed = closed | np.roll(closed, 1, 0) | np.roll(closed, -1, 0) | np.roll(closed, 1, 1) | np.roll(closed, -1, 1)
    for _ in range(2):  # erode back, keeping the outline where it was
        closed = closed & np.roll(closed, 1, 0) & np.roll(closed, -1, 0) & np.roll(closed, 1, 1) & np.roll(closed, -1, 1)
    closed |= mask

    filled, known = normals.copy(), mask.copy()
    for _ in range(iterations):
        missing = closed & ~known
        if not missing.any():
            break
        acc = np.zeros_like(filled)
        cnt = np.zeros(mask.shape, dtype=np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            acc += np.roll(np.roll(filled * known[..., None], dy, 0), dx, 1)
            cnt += np.roll(np.roll(known.astype(np.float32), dy, 0), dx, 1)
        grow = missing & (cnt > 0)
        filled[grow] = acc[grow] / cnt[grow][:, None]
        known |= grow
    return filled, closed & known


def smooth_normals(normals, mask, passes):
    """Average each normal with its neighbours on the head, to calm a noisy scan surface."""
    for _ in range(passes):
        acc = np.zeros_like(normals)
        cnt = np.zeros(mask.shape, dtype=np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                acc += np.roll(np.roll(normals * mask[..., None], dy, 0), dx, 1)
                cnt += np.roll(np.roll(mask.astype(np.float32), dy, 0), dx, 1)
        normals = np.where(mask[..., None], acc / np.maximum(cnt, 1)[..., None], normals)
    return normals


def render_view(hv, hn, center, yaw, pitch, shift=None, under=None, smooth=0):
    """Normals (SIZE,SIZE,3) and mask for the head turned by yaw/pitch degrees, cropped for the dial.
    The left/right centring shift is measured on the first (straight-on) view and reused for the rest.
    `under` is an uncropped (normals, mask) to fill in wherever this scan has nothing."""
    big = SIZE * SUPERSAMPLE
    scale = HEAD_HEIGHT_PX * SUPERSAMPLE / 0.27
    R = rotation(yaw, pitch)
    nimg, mask = splat((hv - center) @ R.T, hn @ R.T, big, scale, np.zeros(3))

    # average down to watch resolution
    nimg = (nimg * mask[..., None]).reshape(SIZE, SUPERSAMPLE, SIZE, SUPERSAMPLE, 3).sum((1, 3))
    count = mask.reshape(SIZE, SUPERSAMPLE, SIZE, SUPERSAMPLE).sum((1, 3))
    mask = count > 0
    nimg[mask] /= count[mask][:, None]
    nimg, mask = fill_holes(nimg, mask)
    nimg = smooth_normals(nimg, mask, smooth)

    # centre the head left-to-right on its own outline, so the circle crop trims both sides evenly
    if shift is None:
        cols = np.flatnonzero(mask[: SIZE // 2 + NARROWING_FROM_CENTER].any(axis=0))
        shift = int(round(SIZE / 2 - (cols[0] + cols[-1] + 1) / 2))
    nimg = np.roll(nimg, shift, axis=1)
    mask = np.roll(mask, shift, axis=1)
    raw = (nimg.copy(), mask.copy())
    if under is not None:
        fill = under[1] & ~mask
        nimg[fill] = under[0][fill]
        mask = mask | fill

    yy, xx = np.mgrid[0:SIZE, 0:SIZE]
    mask &= np.hypot(xx - SIZE / 2 + 0.5, yy - SIZE / 2 + 0.5) <= HEAD_RADIUS
    # from the cheekbones down the outline can only narrow, which trims anything sticking out past the jaw
    left, right = 0, SIZE - 1
    for y in range(SIZE // 2 + NARROWING_FROM_CENTER, SIZE):
        mask[y, :left] = False
        mask[y, right + 1:] = False
        cols = np.flatnonzero(mask[y])
        if len(cols):
            left, right = cols[0], cols[-1]
    nimg /= np.linalg.norm(nimg, axis=-1, keepdims=True) + 1e-9
    nimg[..., 2] = np.maximum(nimg[..., 2], 0.01)  # a visible surface always faces the viewer a little
    nimg[~mask] = 0
    return nimg, mask, shift, raw


def write_views(f, name, views, box):
    bx, by, bw, bh = box
    f.write(f"const int8_t {name}[FACE_PITCH_STEPS * FACE_YAW_STEPS][FACE_BOX_W * FACE_BOX_H * 2] PROGMEM = {{\n")
    for p in range(PITCH_STEPS):
        for y in range(YAW_STEPS):
            nimg, mask = views[p, y]
            px = np.clip(np.round(nimg[by:by + bh, bx:bx + bw, :2] * 127), -127, 127).astype(np.int16)
            px[~mask[by:by + bh, bx:bx + bw], 0] = -128
            flat = px.reshape(-1)
            f.write("  {\n")
            for i in range(0, len(flat), 32):
                f.write("    " + ",".join(str(int(x)) for x in flat[i:i + 32]) + ",\n")
            f.write("  },\n")
    f.write("};\n\n")


def save_preview(name, views):
    tiles = [[np.where(views[p, y][1][..., None], views[p, y][0] * 0.5 + 0.5, 0.1) for y in range(YAW_STEPS)]
             for p in range(PITCH_STEPS)]
    save_png(os.path.join(FACE_DIR, "previews", name), np.concatenate([np.concatenate(r, 1) for r in tiles], 0))


def main():
    v, n, _ = load_obj(os.path.join(FACE_DIR, "me.obj"))
    frame = head_frame(v)
    center = np.array([frame[1], frame[0] - 0.125, frame[2]])
    hv, hn = head_points(v, n, frame)

    # the open-mouth scan, moved onto the first one and cropped the same way
    align = np.load(os.path.join(FACE_DIR, "me-mouth-align.npz"))
    mv, mn, _ = load_obj(os.path.join(FACE_DIR, "me-mouth.obj"))
    mv, mn = head_points(mv @ align["R"].T + align["t"], mn @ align["R"].T, frame)

    shift = render_view(hv, hn, center, 0, 0)[2]
    views, mouth_views = {}, {}
    for p in range(PITCH_STEPS):
        for y in range(YAW_STEPS):
            yaw = (y - YAW_STEPS // 2) * YAW_STEP_DEG
            pitch = (p - PITCH_STEPS // 2) * PITCH_STEP_DEG
            nimg, mask, _, raw = render_view(hv, hn, center, yaw, pitch, shift)
            views[p, y] = (nimg, mask)
            mouth_views[p, y] = render_view(mv, mn, center, yaw, pitch, shift, under=raw, smooth=MOUTH_SMOOTHING)[:2]

    # store only the box every view fits in
    any_mask = np.any([m for _, m in list(views.values()) + list(mouth_views.values())], axis=0)
    rows, cols = np.flatnonzero(any_mask.any(1)), np.flatnonzero(any_mask.any(0))
    bx, by, bw, bh = cols[0], rows[0], cols[-1] - cols[0] + 1, rows[-1] - rows[0] + 1

    out = os.path.join(FACE_DIR, "face_normals.h")
    with open(out, "w") as f:
        f.write("// Generated by tools/make_normal_map.py from me.obj - do not edit by hand.\n")
        f.write("// Surface normals of the head from a grid of angles, inside the box FACE_BOX_* of the 200x200\n")
        f.write("// screen. Each pixel is (x, y) scaled to -127..127 with x right and y up; z (out of the\n")
        f.write("// screen) is sqrt(1 - x^2 - y^2). x == -128 means no head there (background).\n")
        f.write("#pragma once\n\n")
        f.write(f"#define FACE_YAW_STEPS {YAW_STEPS}\n#define FACE_YAW_STEP_DEG {YAW_STEP_DEG}\n")
        f.write(f"#define FACE_PITCH_STEPS {PITCH_STEPS}\n#define FACE_PITCH_STEP_DEG {PITCH_STEP_DEG}\n")
        f.write(f"#define FACE_BOX_X {bx}\n#define FACE_BOX_Y {by}\n#define FACE_BOX_W {bw}\n#define FACE_BOX_H {bh}\n\n")
        f.write("// [pitch * FACE_YAW_STEPS + yaw], straight on is the middle of each range\n")
        write_views(f, "FACE_VIEWS", views, (bx, by, bw, bh))
        f.write("// the same views with the mouth open\n")
        write_views(f, "FACE_VIEWS_MOUTH", mouth_views, (bx, by, bw, bh))

    os.makedirs(os.path.join(FACE_DIR, "previews"), exist_ok=True)
    save_preview("normal-maps.png", views)
    save_preview("normal-maps-mouth.png", mouth_views)
    size_kb = 2 * PITCH_STEPS * YAW_STEPS * bw * bh * 2 / 1024
    print(f"wrote {out}: 2 x {PITCH_STEPS * YAW_STEPS} views, box {bw}x{bh} at ({bx},{by}), {size_kb:.0f} KB")


if __name__ == "__main__":
    main()
