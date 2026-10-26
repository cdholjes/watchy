"""Load the face scan and render it from any angle (numpy only, no 3D libraries).

The scan is dense (~1mm between points), so instead of rasterising triangles we splat
every vertex into a depth buffer, which is fast in numpy and gap-free at watch resolution.
"""
import os
import struct
import zlib

import numpy as np


def load_obj(path, cache=True):
    """Vertices (N,3), vertex normals (N,3) and colours (N,3) from an OBJ; cached as .npz."""
    npz = os.path.splitext(path)[0] + ".npz"
    if cache and os.path.exists(npz) and os.path.getmtime(npz) >= os.path.getmtime(path):
        d = np.load(npz)
        return d["v"], d["n"], d["c"]

    verts, faces = [], []
    with open(path) as f:
        for line in f:
            if line.startswith("v "):
                verts.append(line[2:])
            elif line.startswith("f "):
                # "f a b c" or "f a/t/n ..." - keep the vertex index only
                faces.append(" ".join(p.split("/")[0] for p in line[2:].split()))
    v = np.array(" ".join(verts).split(), dtype=np.float32).reshape(len(verts), -1)
    fidx = np.array(" ".join(faces).split(), dtype=np.int64).reshape(len(faces), -1)[:, :3] - 1
    pos = v[:, :3]
    col = v[:, 3:6] if v.shape[1] >= 6 else np.full_like(pos, 0.7)

    # area-weighted vertex normals from the triangles
    a, b, c = pos[fidx[:, 0]], pos[fidx[:, 1]], pos[fidx[:, 2]]
    fn = np.cross(b - a, c - a)
    n = np.zeros_like(pos)
    for k in range(3):
        for axis in range(3):
            n[:, axis] += np.bincount(fidx[:, k], weights=fn[:, axis], minlength=len(pos))
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12

    if cache:
        np.savez(npz, v=pos, n=n, c=col)
    return pos, n, col


def rotation(yaw=0.0, pitch=0.0, roll=0.0):
    """Rotation matrix from degrees: yaw around Y (turn left/right), pitch around X, roll around Z."""
    y, p, r = np.radians([yaw, pitch, roll])
    ry = np.array([[np.cos(y), 0, np.sin(y)], [0, 1, 0], [-np.sin(y), 0, np.cos(y)]])
    rx = np.array([[1, 0, 0], [0, np.cos(p), -np.sin(p)], [0, np.sin(p), np.cos(p)]])
    rz = np.array([[np.cos(r), -np.sin(r), 0], [np.sin(r), np.cos(r), 0], [0, 0, 1]])
    return rz @ rx @ ry


def splat(pos, values, size, scale, center, splat_px=1):
    """Orthographic render looking down -Z (camera at +Z). Returns (image, mask) for the nearest points."""
    p = (pos - center) * scale
    x = np.round(p[:, 0] + size / 2).astype(np.int64)
    y = np.round(-p[:, 1] + size / 2).astype(np.int64)
    z = p[:, 2]
    img = np.zeros((size, size) + values.shape[1:], dtype=np.float32)
    depth = np.full((size, size), -np.inf, dtype=np.float32)
    order = np.argsort(z)  # far to near, so nearer points overwrite
    for dy in range(-splat_px + 1, splat_px):
        for dx in range(-splat_px + 1, splat_px):
            xx, yy = x[order] + dx, y[order] + dy
            ok = (xx >= 0) & (xx < size) & (yy >= 0) & (yy < size)
            xx, yy, zz, vv = xx[ok], yy[ok], z[order][ok], values[order][ok]
            nearer = zz >= depth[yy, xx]
            depth[yy[nearer], xx[nearer]] = zz[nearer]
            img[yy[nearer], xx[nearer]] = vv[nearer]
    return img, np.isfinite(depth)


def save_png(path, img):
    """Grayscale (H,W) or RGB (H,W,3) floats in 0..1 to PNG."""
    a = (np.clip(img, 0, 1) * 255).astype(np.uint8)
    h, w = a.shape[:2]
    channels = 1 if a.ndim == 2 else 3
    raw = b"".join(b"\x00" + a[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    color_type = 0 if channels == 1 else 2
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
