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
    zs = np.asarray(traj_z, dtype=np.float32)
    view_w = w - 40
    view_h = h - 60
    ox, oy = 20, 40
    cx = 0.5 * (float(xs.min()) + float(xs.max()))
    cz = 0.5 * (float(zs.min()) + float(zs.max()))
    rx = max(1e-6, 0.5 * (float(xs.max()) - float(xs.min())))
    rz = max(1e-6, 0.5 * (float(zs.max()) - float(zs.min())))
    # Isotropic scaling: same ratio on x/z to avoid directional stretch.
    r = max(rx, rz)
    r *= 1.1  # pad 10%

    sx = (xs - cx) / r
    sz = (zs - cz) / r
    px = ox + (sx * 0.5 + 0.5) * view_w
    py = oy + (0.5 - sz * 0.5) * view_h
    pts = np.stack([px.astype(np.int32), py.astype(np.int32)], axis=1)

    cv2.rectangle(canvas, (ox, oy), (ox + view_w, oy + view_h), (70, 70, 70), 1)
    cv2.polylines(canvas, [pts], isClosed=False, color=(255, 180, 0), thickness=2)
    cv2.circle(canvas, tuple(pts[-1]), 4, (0, 0, 255), -1)
    return canvas


def ensure_size_nearest(img: np.ndarray, ref_w: int, ref_h: int) -> np.ndarray:
    if img.shape[0] == ref_h and img.shape[1] == ref_w:
        return img
    return resize_nearest(img, ref_w, ref_h)


def make_triptych_panel(rgb_bgr: np.ndarray, depth_bgr: np.ndarray, traj_bgr: np.ndarray) -> np.ndarray:
    h, w = rgb_bgr.shape[:2]
    depth_bgr = ensure_size_nearest(depth_bgr, w, h)
    traj_bgr = ensure_size_nearest(traj_bgr, w, h)
    return np.hstack([rgb_bgr, depth_bgr, traj_bgr])


def draw_header_text(img: np.ndarray, text: str) -> None:
    cv2.putText(img, text, (12, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2, cv2.LINE_AA)
