#!/usr/bin/env python3
"""Line the open-mouth scan up with the original one, so both can be baked from the same angles.

    python3 align_scans.py        # writes ../me-mouth-align.npz and ../previews/align.png

The two scans come from separate captures, so they sit in different positions and the head is tilted
differently. This finds the rigid move (rotation + translation) that best lays the open-mouth scan's
upper face (forehead, eyes, nose: the parts that don't change when the mouth opens) onto the original,
with ICP: repeatedly pair each point with its nearest neighbour and solve for the best fit.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from mesh import load_obj, rotation, save_png, splat  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FACE_DIR = os.path.dirname(HERE)

UPPER_FACE = (0.03, 0.13)  # metres below the crown that count as upper face (brow to under the nose)
SAMPLES = 4000
ITERATIONS = 40
KEEP = 0.8  # fraction of closest pairs used each round, so the odd mismatch doesn't drag the fit


def upper_face(v, n, top, cx, cz):
    """Front-facing points between the brow and the nose, around the head's centre line."""
    keep = (v[:, 1] < top - UPPER_FACE[0]) & (v[:, 1] > top - UPPER_FACE[1])
    keep &= np.hypot(v[:, 0] - cx, v[:, 2] - cz) < 0.14
    keep &= n[:, 2] > 0.3
    return v[keep]


def nearest(src, dst, chunk=500):
    """Index into dst of each src point's nearest neighbour, and the distance (brute force, chunked)."""
    idx = np.empty(len(src), dtype=np.int64)
    dist = np.empty(len(src))
    dd = (dst ** 2).sum(1)
    for i in range(0, len(src), chunk):
        s = src[i:i + chunk]
        d = (s ** 2).sum(1)[:, None] - 2 * s @ dst.T + dd[None, :]
        idx[i:i + chunk] = d.argmin(1)
        dist[i:i + chunk] = np.sqrt(np.maximum(d[np.arange(len(s)), idx[i:i + chunk]], 0))
    return idx, dist


def best_fit(a, b):
    """Rotation R and translation t minimising |R a + t - b| (Kabsch)."""
    ca, cb = a.mean(0), b.mean(0)
    u, _, vt = np.linalg.svd((a - ca).T @ (b - cb))
    d = np.sign(np.linalg.det(vt.T @ u.T))
    R = vt.T @ np.diag([1, 1, d]) @ u.T
    return R, cb - R @ ca


def icp(src, dst, R, t):
    for _ in range(ITERATIONS):
        moved = src @ R.T + t
        idx, dist = nearest(moved, dst)
        good = dist <= np.quantile(dist, KEEP)
        dR, dt = best_fit(moved[good], dst[idx[good]])
        R, t = dR @ R, dR @ t + dt
    moved = src @ R.T + t
    _, dist = nearest(moved, dst)
    return R, t, np.sort(dist)[: int(len(dist) * KEEP)].mean()


def main():
    rng = np.random.default_rng(0)
    v0, n0, _ = load_obj(os.path.join(FACE_DIR, "me.obj"))
    v1, n1, _ = load_obj(os.path.join(FACE_DIR, "me-mouth.obj"))

    def frame(v):
        top = v[:, 1].max()
        crown = v[v[:, 1] > top - 0.10]
        return top, np.median(crown[:, 0]), np.median(crown[:, 2])

    dst = upper_face(v0, n0, *frame(v0))
    src = upper_face(v1, n1, *frame(v1))
    dst = dst[rng.choice(len(dst), min(len(dst), 4 * SAMPLES), replace=False)]
    src = src[rng.choice(len(src), min(len(src), SAMPLES), replace=False)]
    print(f"upper face points: original {len(dst)}, open mouth {len(src)}")

    # the scans are tilted differently, so start ICP from a few head tilts and keep the best
    best = None
    for pitch in (-30, -15, 0, 15, 30):
        for yaw in (-15, 0, 15):
            R = rotation(yaw, pitch)
            t = dst.mean(0) - R @ src.mean(0)
            R, t, err = icp(src, dst, R, t)
            print(f"  start yaw {yaw:+} pitch {pitch:+}: error {err * 1000:.2f} mm")
            if best is None or err < best[2]:
                best = (R, t, err)
    R, t, err = best
    print(f"best fit: {err * 1000:.2f} mm average over the closest {KEEP:.0%}")
    np.savez(os.path.join(FACE_DIR, "me-mouth-align.npz"), R=R, t=t)

    # preview: original, open mouth moved onto it, and the two overlaid (red = original, green = mouth)
    ctr = v0[v0[:, 1] > v0[:, 1].max() - 0.26].mean(0)
    moved = v1 @ R.T + t
    tiles = []
    for yaw in (0, 60):
        rot = rotation(yaw, 0)
        a, ma = splat((v0 - ctr) @ rot.T, np.ones((len(v0), 3)), 300, 300 / 0.35, np.zeros(3))
        b, mb = splat((moved - ctr) @ rot.T, np.ones((len(moved), 3)), 300, 300 / 0.35, np.zeros(3))
        overlay = np.stack([ma, mb, np.zeros_like(ma)], -1).astype(np.float32) * 0.9 + 0.05
        tiles.append(overlay)
    os.makedirs(os.path.join(FACE_DIR, "previews"), exist_ok=True)
    save_png(os.path.join(FACE_DIR, "previews", "align.png"), np.concatenate(tiles, 1))


if __name__ == "__main__":
    main()
