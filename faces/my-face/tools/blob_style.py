"""Shade like The Blob: a matcap (picture of a lit chrome ball) looked up by surface normal,
then turned into black/white dots by comparing against a blue-noise pattern."""
import os
import re

import numpy as np

BLOB_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "the-blob")


def load_header_bytes(name):
    """200x200 8-bit image stored as a C array in one of The Blob's headers."""
    text = open(os.path.join(BLOB_DIR, name)).read()
    data = np.array([int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", text)], dtype=np.uint8)
    return data[:200 * 200].reshape(200, 200)


MATCAP = load_header_bytes("MatCapSource.h")
BLUE_NOISE = load_header_bytes("BlueNoise200.h")


def shade(normals, mask):
    """normals: (H,W,3) in view space (x right, y up, z toward the viewer). Returns matcap brightness 0..255."""
    u = np.clip(np.round(100 + normals[..., 0] * 99), 0, 199).astype(int)
    v = np.clip(np.round(100 - normals[..., 1] * 99), 0, 199).astype(int)
    out = MATCAP[v, u].astype(np.float32)
    out[~mask] = 0
    return out


def dither(brightness, threshold_offset=0):
    """Blue-noise dither to 1-bit (True = white), same rule as The Blob."""
    h, w = brightness.shape
    noise = np.tile(BLUE_NOISE, (h // 200 + 1, w // 200 + 1))[:h, :w].astype(np.float32)
    return brightness > noise + threshold_offset
