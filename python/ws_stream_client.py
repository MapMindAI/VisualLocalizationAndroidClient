#!/usr/bin/env python3
import argparse
import struct
import time
from dataclasses import dataclass
from typing import Optional, Tuple

import cv2  # type: ignore
import grpc  # type: ignore
import numpy as np  # type: ignore


FRAME_HEADER_SIZE = 64
FRAME_MAGIC_ASCII = b"VLP2"
FRAME_MAGIC_LEGACY = 0x564C5032
DEPTH_TAG = b"DPT1"
STREAM_METHOD = "/vlp.FrameStreamService/StreamFrames"


@dataclass
class FramePacket:
    timestamp_ns: int
    width: int
    height: int
    fx: float
    fy: float
    cx: float
    cy: float
    qx: float
    qy: float
    qz: float
    qw: float
    tx: float
    ty: float
    tz: float
    jpeg: bytes
    depth_ts_ns: int = 0
    depth_w: int = 0
    depth_h: int = 0
    depth_bytes: bytes = b""


def parse_packet(payload: bytes) -> FramePacket:
    if len(payload) < FRAME_HEADER_SIZE:
        raise ValueError(f"payload too small: {len(payload)}")
    magic_u32 = struct.unpack_from("<I", payload, 0)[0]
    if payload[0:4] == FRAME_MAGIC_ASCII:
        # ASCII-tagged variant.
        timestamp_ns = struct.unpack_from("<Q", payload, 8)[0]
        width = struct.unpack_from("<I", payload, 16)[0]
        height = struct.unpack_from("<I", payload, 20)[0]
        fx, fy, cx, cy = struct.unpack_from("<4f", payload, 24)
        qx, qy, qz, qw, tx, ty, tz = struct.unpack_from("<7f", payload, 40)
    elif magic_u32 == FRAME_MAGIC_LEGACY:
        # Legacy packed 64-byte header written by mapmind_vlp_api.cc.
        timestamp_ns = struct.unpack_from("<Q", payload, 4)[0]
        width = struct.unpack_from("<I", payload, 12)[0]
        height = struct.unpack_from("<I", payload, 16)[0]
        fx, fy, cx, cy = struct.unpack_from("<4f", payload, 20)
        qx, qy, qz, qw, tx, ty, tz = struct.unpack_from("<7f", payload, 36)
    else:
        raise ValueError(f"bad magic: {payload[0:4]!r}")
    body = payload[FRAME_HEADER_SIZE:]

    depth_ts_ns = 0
    depth_w = 0
    depth_h = 0
    depth_bytes = b""

    soi = body[0:2]
    if soi != b"\xff\xd8":
        raise ValueError("body is not JPEG")
    eoi_idx = body.rfind(b"\xff\xd9")
    if eoi_idx < 0:
        raise ValueError("JPEG EOI marker not found")
    jpeg_end = eoi_idx + 2
    jpeg = body[:jpeg_end]
    trailer = body[jpeg_end:]
    if len(trailer) >= 24 and trailer[0:4] == DEPTH_TAG:
        depth_ts_ns = struct.unpack_from("<Q", trailer, 4)[0]
        depth_w = struct.unpack_from("<I", trailer, 12)[0]
        depth_h = struct.unpack_from("<I", trailer, 16)[0]
        depth_sz = struct.unpack_from("<I", trailer, 20)[0]
        if len(trailer) >= 24 + depth_sz:
            depth_bytes = trailer[24 : 24 + depth_sz]

    return FramePacket(
        timestamp_ns=timestamp_ns,
        width=width,
        height=height,
        fx=fx,
        fy=fy,
        cx=cx,
        cy=cy,
        qx=qx,
        qy=qy,
        qz=qz,
        qw=qw,
        tx=tx,
        ty=ty,
        tz=tz,
        jpeg=jpeg,
        depth_ts_ns=depth_ts_ns,
        depth_w=depth_w,
        depth_h=depth_h,
        depth_bytes=depth_bytes,
    )


def decode_jpeg(jpeg: bytes):
    arr = np.frombuffer(jpeg, dtype=np.uint8)
    return cv2.imdecode(arr, cv2.IMREAD_COLOR)


def depth_preview(depth_bytes: bytes, w: int, h: int):
    if not depth_bytes or w <= 0 or h <= 0:
        return None
    expected = w * h * 2
    if len(depth_bytes) < expected:
        return None
    d16 = np.frombuffer(depth_bytes[:expected], dtype=np.uint16).reshape((h, w))
    # Depth payload stores two bytes per pixel (low/high); visualize normalized intensity.
    d8 = cv2.convertScaleAbs(d16, alpha=255.0 / max(1.0, float(np.percentile(d16, 99))))
    return cv2.applyColorMap(d8, cv2.COLORMAP_TURBO)


def draw_overlay(img, pkt: FramePacket, fps: float):
    lines = [
        f"ts={pkt.timestamp_ns} fps={fps:.2f}",
        f"size={pkt.width}x{pkt.height} jpeg={len(pkt.jpeg)}",
        f"fx={pkt.fx:.2f} fy={pkt.fy:.2f} cx={pkt.cx:.2f} cy={pkt.cy:.2f}",
        f"q=({pkt.qx:.3f},{pkt.qy:.3f},{pkt.qz:.3f},{pkt.qw:.3f})",
        f"t=({pkt.tx:.3f},{pkt.ty:.3f},{pkt.tz:.3f})",
        f"depth={'yes' if pkt.depth_ts_ns else 'no'} dts={pkt.depth_ts_ns}",
        "keys: q=quit",
    ]
    y = 24
    for line in lines:
        cv2.putText(img, line, (10, y), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 255, 0), 2, cv2.LINE_AA)
        y += 22


def stream_frames(host: str, port: int, timeout_s: float):
    target = f"{host}:{port}"
    channel = grpc.insecure_channel(target)
    call = channel.unary_stream(
        STREAM_METHOD,
        request_serializer=lambda b: b,
        response_deserializer=lambda b: b,
    )
    return call(b"subscribe", timeout=timeout_s)


def main() -> int:
    ap = argparse.ArgumentParser(description="Python gRPC client for mapmind VLP stream")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=50051)
    ap.add_argument("--timeout", type=float, default=3600.0)
    ap.add_argument("--show-depth", action="store_true", help="show depth preview window when depth trailer exists")
    args = ap.parse_args()

    last_t = time.time()
    fps = 0.0
    latest_depth_img = None

    for payload in stream_frames(args.host, args.port, args.timeout):
        pkt = parse_packet(payload)
        img = decode_jpeg(pkt.jpeg)
        if img is None:
            continue

        now = time.time()
        dt = max(1e-6, now - last_t)
        fps = 0.9 * fps + 0.1 * (1.0 / dt) if fps > 0 else (1.0 / dt)
        last_t = now

        depth_img = None
        if pkt.depth_ts_ns:
            depth_img = depth_preview(pkt.depth_bytes, pkt.depth_w, pkt.depth_h)
        if depth_img is not None:
            depth_img = cv2.resize(depth_img, (img.shape[1], img.shape[0]), interpolation=cv2.INTER_NEAREST)
            latest_depth_img = depth_img
        elif latest_depth_img is not None:
            depth_img = latest_depth_img
        else:
            depth_img = np.zeros_like(img)

        draw_overlay(img, pkt, fps)
        cv2.putText(depth_img, "depth", (10, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2, cv2.LINE_AA)
        side_by_side = np.hstack([img, depth_img])
        cv2.imshow("vlp_stream_rgb_depth", side_by_side)

        key = cv2.waitKey(1) & 0xFF
        if key == ord("q"):
            break

    cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
