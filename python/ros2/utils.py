import io
from typing import Optional

import cv2  # type: ignore
import numpy as np  # type: ignore
from PIL import Image  # type: ignore


def decode_compressed_image_rgb(raw: bytes) -> Optional[np.ndarray]:
    if not raw:
        return None
    try:
        return np.asarray(Image.open(io.BytesIO(raw)).convert("RGB"))
    except Exception:
        return None


def depth_to_rgb(raw: bytes, width: int, height: int, encoding: str, turbo_cmap) -> Optional[np.ndarray]:
    w = int(width)
    h = int(height)
    if w <= 0 or h <= 0:
        return None
    enc = str(encoding).lower()

    if enc in ("16uc1", "mono16"):
        expected = w * h * 2
        if len(raw) < expected:
            return None
        d = np.frombuffer(raw[:expected], dtype=np.uint16).reshape((h, w)).astype(np.float32)
    elif enc == "32fc1":
        expected = w * h * 4
        if len(raw) < expected:
            return None
        d = np.frombuffer(raw[:expected], dtype=np.float32).reshape((h, w))
        d = np.nan_to_num(d, nan=0.0, posinf=0.0, neginf=0.0)
    else:
        return None

    p99 = float(np.percentile(d, 99))
    dnorm = np.clip(d / max(1.0, p99), 0.0, 1.0)
    return (turbo_cmap(dnorm)[..., :3] * 255.0).astype(np.uint8)


def rot90_ccw(img: np.ndarray) -> np.ndarray:
    return np.rot90(img, 1)


def resize_nearest(img: np.ndarray, w: int, h: int) -> np.ndarray:
    pil = Image.fromarray(img)
    if hasattr(Image, "Resampling"):
        return np.asarray(pil.resize((w, h), Image.Resampling.NEAREST))
    return np.asarray(pil.resize((w, h), Image.NEAREST))


def depth_msg_to_bgr_turbo(raw: bytes, width: int, height: int, encoding: str) -> Optional[np.ndarray]:
    rgb = depth_to_rgb(raw, width, height, encoding, lambda x: np.dstack([x, x, x, np.ones_like(x)]))
    if rgb is None:
        return None
    return cv2.applyColorMap(rgb[..., 0].astype(np.uint8), cv2.COLORMAP_TURBO)


def draw_traj_canvas(traj_x, traj_z, w: int, h: int) -> np.ndarray:
    canvas = np.zeros((h, w, 3), dtype=np.uint8)
    cv2.putText(
        canvas,
        "VIO Trajectory (x-right, y-up)",
        (10, 28),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (220, 220, 220),
        2,
        cv2.LINE_AA,
    )
    if len(traj_x) < 2:
        return canvas

    xs = np.asarray(traj_x, dtype=np.float32)
    zs = -np.asarray(traj_z, dtype=np.float32)
    x_min, x_max = float(xs.min()), float(xs.max())
    z_min, z_max = float(zs.min()), float(zs.max())
    span = max(x_max - x_min, z_max - z_min, 1e-3)
    pad = 0.1 * span
    x_min -= pad
    x_max += pad
    z_min -= pad
    z_max += pad

    view_w = w - 40
    view_h = h - 60
    ox, oy = 20, 40

    px = (xs - x_min) / max(1e-6, (x_max - x_min))
    pz = (zs - z_min) / max(1e-6, (z_max - z_min))
    pts = np.stack(
        [
            (ox + px * view_w).astype(np.int32),
            (oy + (1.0 - pz) * view_h).astype(np.int32),
        ],
        axis=1,
    )

    cv2.rectangle(canvas, (ox, oy), (ox + view_w, oy + view_h), (70, 70, 70), 1)
    cv2.polylines(canvas, [pts], isClosed=False, color=(255, 180, 0), thickness=2)
    cv2.circle(canvas, tuple(pts[-1]), 4, (0, 0, 255), -1)
    return canvas
