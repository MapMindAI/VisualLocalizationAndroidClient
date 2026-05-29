#!/usr/bin/env python3
import argparse
import os
import struct
from dataclasses import dataclass
from typing import Optional

import numpy as np  # type: ignore
import rosbag2_py  # type: ignore
from rclpy.serialization import deserialize_message  # type: ignore
from rosidl_runtime_py.utilities import get_message  # type: ignore


FILE_MAGIC = b"VLPREC1\n"
FILE_VERSION = 1
FRAME_MAGIC_VLP2 = 0x564C5032
DEPTH_TAG = b"DPT2"

# Requested default depth intrinsics from user.
DEFAULT_DEPTH_FX = 313.94085693359375
DEFAULT_DEPTH_FY = 313.94085693359375
DEFAULT_DEPTH_CX = 269.742431640625
DEFAULT_DEPTH_CY = 316.34063720703125


@dataclass
class Intrinsics:
    fx: float
    fy: float
    cx: float
    cy: float
    width: int = 0
    height: int = 0
    stamp_ns: int = 0


@dataclass
class PoseState:
    stamp_ns: int
    qx: float
    qy: float
    qz: float
    qw: float
    tx: float
    ty: float
    tz: float


@dataclass
class DepthState:
    stamp_ns: int
    width: int
    height: int
    bytes_u16_mm_le: bytes
    intr: Intrinsics


def _stamp_to_ns(stamp) -> int:
    return int(stamp.sec) * 1_000_000_000 + int(stamp.nanosec)


def _normalize_quat(x: float, y: float, z: float, w: float):
    n2 = x * x + y * y + z * z + w * w
    if n2 <= 1e-20:
        return 0.0, 0.0, 0.0, 1.0
    inv = n2 ** -0.5
    return x * inv, y * inv, z * inv, w * inv


def _quat_mul(ax: float, ay: float, az: float, aw: float, bx: float, by: float, bz: float, bw: float):
    x = aw * bx + ax * bw + ay * bz - az * by
    y = aw * by - ax * bz + ay * bw + az * bx
    z = aw * bz + ax * by - ay * bx + az * bw
    w = aw * bw - ax * bx - ay * by - az * bz
    return x, y, z, w


def _apply_camera_to_opengl_pose_transform(
    qx: float, qy: float, qz: float, qw: float, tx: float, ty: float, tz: float
):
    # Same logic as mapmind_vlp_api.cc
    qx, qy, qz, qw = _normalize_quat(qx, qy, qz, qw)
    qx, qy, qz, qw = _quat_mul(qx, qy, qz, qw, 1.0, 0.0, 0.0, 0.0)
    qx, qy, qz, qw = _normalize_quat(qx, qy, qz, qw)
    return qx, qy, qz, qw, tx, ty, tz


def _parse_camera_info(msg) -> Intrinsics:
    k = list(msg.k)
    fx, fy, cx, cy = float(k[0]), float(k[4]), float(k[2]), float(k[5])
    return Intrinsics(
        fx=fx,
        fy=fy,
        cx=cx,
        cy=cy,
        width=int(msg.width),
        height=int(msg.height),
        stamp_ns=_stamp_to_ns(msg.header.stamp),
    )


def _depth_image_to_u16_mm_le(msg) -> Optional[bytes]:
    w = int(msg.width)
    h = int(msg.height)
    if w <= 0 or h <= 0:
        return None
    raw = bytes(msg.data)
    step = int(msg.step)
    enc = str(msg.encoding).lower()

    if enc in ("16uc1", "mono16"):
        row_bytes = w * 2
        if step <= 0:
            step = row_bytes
        if len(raw) < step * h:
            return None
        if step == row_bytes:
            out = raw[: row_bytes * h]
        else:
            rows = [raw[r * step : r * step + row_bytes] for r in range(h)]
            out = b"".join(rows)
        if int(msg.is_bigendian) != 0:
            arr = np.frombuffer(out, dtype=">u2").astype("<u2", copy=False)
            return arr.tobytes()
        return out

    if enc == "32fc1":
        row_bytes = w * 4
        if step <= 0:
            step = row_bytes
        if len(raw) < step * h:
            return None
        if step == row_bytes:
            fraw = raw[: row_bytes * h]
        else:
            rows = [raw[r * step : r * step + row_bytes] for r in range(h)]
            fraw = b"".join(rows)
        dtype = ">f4" if int(msg.is_bigendian) != 0 else "<f4"
        d = np.frombuffer(fraw, dtype=dtype).astype(np.float32)
        d = np.nan_to_num(d, nan=0.0, posinf=0.0, neginf=0.0)
        mm = np.clip(np.round(d * 1000.0), 0, 65535).astype("<u2")
        return mm.tobytes()

    return None


def _derive_depth_intr_from_rgb(depth_w: int, depth_h: int, rgb_intr: Optional[Intrinsics]) -> Intrinsics:
    if rgb_intr is None or rgb_intr.width <= 0 or rgb_intr.height <= 0:
        return Intrinsics(
            fx=DEFAULT_DEPTH_FX,
            fy=DEFAULT_DEPTH_FY,
            cx=DEFAULT_DEPTH_CX,
            cy=DEFAULT_DEPTH_CY,
            width=depth_w,
            height=depth_h,
        )
    sx = float(depth_w) / float(rgb_intr.width)
    sy = float(depth_h) / float(rgb_intr.height)
    return Intrinsics(
        fx=rgb_intr.fx * sx,
        fy=rgb_intr.fy * sy,
        cx=rgb_intr.cx * sx,
        cy=rgb_intr.cy * sy,
        width=depth_w,
        height=depth_h,
    )


def _build_vlp2_payload(
    timestamp_ns: int,
    width: int,
    height: int,
    intr: Intrinsics,
    qx: float,
    qy: float,
    qz: float,
    qw: float,
    tx: float,
    ty: float,
    tz: float,
    jpeg_bytes: bytes,
    depth_state: Optional[DepthState],
) -> bytes:
    header = struct.pack(
        "<IQII11f",
        FRAME_MAGIC_VLP2,
        int(timestamp_ns),
        int(width),
        int(height),
        float(intr.fx),
        float(intr.fy),
        float(intr.cx),
        float(intr.cy),
        float(qx),
        float(qy),
        float(qz),
        float(qw),
        float(tx),
        float(ty),
        float(tz),
    )
    payload = bytearray(header)
    payload.extend(jpeg_bytes)
    if depth_state is not None:
        depth_bytes = depth_state.bytes_u16_mm_le
        payload.extend(DEPTH_TAG)
        payload.extend(
            struct.pack(
                "<QIII4f",
                int(depth_state.stamp_ns),
                depth_state.width,
                depth_state.height,
                len(depth_bytes),
                float(depth_state.intr.fx),
                float(depth_state.intr.fy),
                float(depth_state.intr.cx),
                float(depth_state.intr.cy),
            )
        )
        payload.extend(depth_bytes)
    return bytes(payload)


def main() -> int:
    ap = argparse.ArgumentParser(description="Convert ROS2 bag to VLPREC1 (.rec) format.")
    ap.add_argument("--bag", required=True, help="ROS2 bag directory containing metadata.yaml")
    ap.add_argument("--output", required=True, help="Output .rec path")
    ap.add_argument("--color-topic", default="/camera/camera/color/image_rect_raw/compressed")
    ap.add_argument("--depth-topic", default="/camera/camera/depth/image_rect_raw")
    ap.add_argument("--vio-topic", default="/camera/camera/vio_100hz")
    ap.add_argument("--color-info-topic", default="/camera/camera/color/camera_info")
    ap.add_argument("--depth-info-topic", default="/camera/camera/depth/camera_info")
    ap.add_argument("--max-frames", type=int, default=0)
    ap.add_argument("--log-every", type=int, default=60)
    args = ap.parse_args()

    storage_options = rosbag2_py.StorageOptions(uri=args.bag, storage_id="sqlite3")
    converter_options = rosbag2_py.ConverterOptions("cdr", "cdr")
    reader = rosbag2_py.SequentialReader()
    reader.open(storage_options, converter_options)

    type_map = {t.name: t.type for t in reader.get_all_topics_and_types()}
    if args.color_topic not in type_map:
        raise ValueError(f"Missing color topic: {args.color_topic}")
    if args.depth_topic not in type_map:
        raise ValueError(f"Missing depth topic: {args.depth_topic}")
    if args.vio_topic not in type_map:
        raise ValueError(f"Missing pose topic: {args.vio_topic}")

    msg_types = {name: get_message(typename) for name, typename in type_map.items()}
    interested = {
        args.color_topic,
        args.depth_topic,
        args.vio_topic,
        args.color_info_topic,
        args.depth_info_topic,
    }

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    out = open(args.output, "wb")
    out.write(struct.pack("<8sI", FILE_MAGIC, FILE_VERSION))

    latest_rgb_intr: Optional[Intrinsics] = None
    latest_depth_intr: Optional[Intrinsics] = Intrinsics(
        fx=DEFAULT_DEPTH_FX,
        fy=DEFAULT_DEPTH_FY,
        cx=DEFAULT_DEPTH_CX,
        cy=DEFAULT_DEPTH_CY,
    )
    latest_pose: Optional[PoseState] = None
    latest_depth: Optional[DepthState] = None

    first_ts_ns: Optional[int] = None
    written = 0
    skipped_no_pose = 0
    skipped_no_depth = 0

    while reader.has_next():
        topic, data, _t_ns = reader.read_next()
        if topic not in interested:
            continue
        msg = deserialize_message(data, msg_types[topic])

        if topic == args.color_info_topic:
            latest_rgb_intr = _parse_camera_info(msg)
            continue
        if topic == args.depth_info_topic:
            latest_depth_intr = _parse_camera_info(msg)
            continue
        if topic == args.vio_topic:
            p = msg.pose.position
            q = msg.pose.orientation
            latest_pose = PoseState(
                stamp_ns=_stamp_to_ns(msg.header.stamp),
                qx=float(q.x),
                qy=float(q.y),
                qz=float(q.z),
                qw=float(q.w),
                tx=float(p.x),
                ty=float(p.y),
                tz=float(p.z),
            )
            continue
        if topic == args.depth_topic:
            db = _depth_image_to_u16_mm_le(msg)
            if db is None:
                continue
            depth_w = int(msg.width)
            depth_h = int(msg.height)
            depth_intr = latest_depth_intr
            if depth_intr is None or depth_intr.width <= 0 or depth_intr.height <= 0:
                depth_intr = _derive_depth_intr_from_rgb(depth_w, depth_h, latest_rgb_intr)
            latest_depth = DepthState(
                stamp_ns=_stamp_to_ns(msg.header.stamp),
                width=depth_w,
                height=depth_h,
                bytes_u16_mm_le=db,
                intr=depth_intr,
            )
            continue

        if topic != args.color_topic:
            continue

        # Color frame path: emit one record when pose/depth are available.
        if latest_pose is None:
            skipped_no_pose += 1
            continue
        if latest_depth is None:
            skipped_no_depth += 1
            continue

        jpeg_bytes = bytes(msg.data)
        if len(jpeg_bytes) < 4 or jpeg_bytes[0:2] != b"\xff\xd8" or jpeg_bytes[-2:] != b"\xff\xd9":
            # Skip non-JPEG compressed frames.
            continue

        color_ts_ns = _stamp_to_ns(msg.header.stamp)
        if first_ts_ns is None:
            first_ts_ns = color_ts_ns
        rel_ns = max(0, color_ts_ns - first_ts_ns)

        qx, qy, qz, qw, tx, ty, tz = _apply_camera_to_opengl_pose_transform(
            latest_pose.qx,
            latest_pose.qy,
            latest_pose.qz,
            latest_pose.qw,
            latest_pose.tx,
            latest_pose.ty,
            latest_pose.tz,
        )

        # Header intrinsics are RGB intrinsics (when available).
        if latest_rgb_intr is not None and latest_rgb_intr.fx > 0.0 and latest_rgb_intr.fy > 0.0:
          intr = latest_rgb_intr
        else:
          intr = latest_depth.intr
        # Width/height in header: use color camera info if available, otherwise depth dims.
        if latest_rgb_intr is not None and latest_rgb_intr.width > 0 and latest_rgb_intr.height > 0:
            width = latest_rgb_intr.width
            height = latest_rgb_intr.height
        else:
            width = latest_depth.width
            height = latest_depth.height

        payload = _build_vlp2_payload(
            timestamp_ns=color_ts_ns,
            width=width,
            height=height,
            intr=intr,
            qx=qx,
            qy=qy,
            qz=qz,
            qw=qw,
            tx=tx,
            ty=ty,
            tz=tz,
            jpeg_bytes=jpeg_bytes,
            depth_state=latest_depth,
        )

        out.write(struct.pack("<QI", rel_ns, len(payload)))
        out.write(payload)
        written += 1

        if args.log_every > 0 and (written == 1 or written % args.log_every == 0):
            print(
                f"[{written:06d}] rel={rel_ns}ns ts={color_ts_ns} "
                f"img={width}x{height} depth={latest_depth.width}x{latest_depth.height} "
                f"intr(fx,fy,cx,cy)=({intr.fx:.4f},{intr.fy:.4f},{intr.cx:.4f},{intr.cy:.4f})"
            )

        if args.max_frames > 0 and written >= args.max_frames:
            break

    out.close()
    print("----")
    print(f"written_frames: {written}")
    print(f"skipped_no_pose: {skipped_no_pose}")
    print(f"skipped_no_depth: {skipped_no_depth}")
    print(f"output: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
