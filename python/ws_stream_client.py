#!/usr/bin/env python3
import argparse
import math
import os
import struct
import sys
import time
from dataclasses import dataclass
from typing import Any, Dict, Optional

import cv2
import grpc
import numpy as np

from da3_pair_processor import DA3PairProcessor


FRAME_MAGIC = 0x564C5032  # "VLP2"
FRAME_HEADER_STRUCT = struct.Struct("<IQII11f")
FRAME_HEADER_SIZE = FRAME_HEADER_STRUCT.size


@dataclass
class FramePose:
    qx: float
    qy: float
    qz: float
    qw: float
    tx: float
    ty: float
    tz: float


@dataclass
class Keyframe:
    idx: int
    timestamp_ns: int
    image_bgr: np.ndarray
    pose: FramePose


def _scale_from_translation_delta(
    da3_outputs: Dict[str, np.ndarray], pose1: FramePose, pose2: FramePose
) -> Optional[float]:
    if "extrinsics" not in da3_outputs:
        return None
    ex = da3_outputs["extrinsics"]  # expected [1, 2, 3, 4]
    if ex.ndim != 4 or ex.shape[1] < 2 or ex.shape[2] != 3 or ex.shape[3] != 4:
        return None
    t1_da3 = ex[0, 0, :, 3].astype(np.float64)
    t2_da3 = ex[0, 1, :, 3].astype(np.float64)
    da3_delta = float(np.linalg.norm(t2_da3 - t1_da3))
    if not np.isfinite(da3_delta) or da3_delta <= 1e-8:
        return None

    real_delta = _trans_delta(pose1, pose2)
    if not np.isfinite(real_delta) or real_delta <= 1e-8:
        return None
    return float(real_delta / da3_delta)


def _quat_angle_deg(a: FramePose, b: FramePose) -> float:
    q1 = np.array([a.qx, a.qy, a.qz, a.qw], dtype=np.float64)
    q2 = np.array([b.qx, b.qy, b.qz, b.qw], dtype=np.float64)
    n1 = np.linalg.norm(q1)
    n2 = np.linalg.norm(q2)
    if n1 == 0 or n2 == 0:
        return 0.0
    q1 /= n1
    q2 /= n2
    dot = float(np.clip(np.abs(np.dot(q1, q2)), -1.0, 1.0))
    angle_rad = 2.0 * math.acos(dot)
    return math.degrees(angle_rad)


def _trans_delta(a: FramePose, b: FramePose) -> float:
    t1 = np.array([a.tx, a.ty, a.tz], dtype=np.float64)
    t2 = np.array([b.tx, b.ty, b.tz], dtype=np.float64)
    return float(np.linalg.norm(t2 - t1))


def draw_overlay(
    bgr: np.ndarray,
    timestamp_ns: int,
    intrinsics: Dict[str, Any],
    pose: Dict[str, Any],
    fps: float,
) -> np.ndarray:
    height, width = bgr.shape[:2]

    fx = float(intrinsics["fx"])
    fy = float(intrinsics["fy"])
    cx = float(intrinsics["cx"])
    cy = float(intrinsics["cy"])

    qx = float(pose["qx"])
    qy = float(pose["qy"])
    qz = float(pose["qz"])
    qw = float(pose["qw"])
    tx = float(pose["tx"])
    ty = float(pose["ty"])
    tz = float(pose["tz"])

    # Draw principal point.
    pp_x = int(round(cx))
    pp_y = int(round(cy))
    if 0 <= pp_x < width and 0 <= pp_y < height:
        cv2.drawMarker(
            bgr,
            (pp_x, pp_y),
            (0, 255, 255),
            markerType=cv2.MARKER_CROSS,
            markerSize=24,
            thickness=2,
        )
        cv2.circle(bgr, (pp_x, pp_y), 12, (0, 255, 255), 1)

    lines = [
        f"ts(ns): {timestamp_ns}",
        f"fps: {fps:.2f}",
        f"intrinsics fx={fx:.2f} fy={fy:.2f} cx={cx:.2f} cy={cy:.2f}",
        f"pose q=({qx:.4f}, {qy:.4f}, {qz:.4f}, {qw:.4f})",
        f"pose t=({tx:.4f}, {ty:.4f}, {tz:.4f})",
        "press q to quit",
    ]
    y = 28
    for text in lines:
        cv2.putText(
            bgr,
            text,
            (10, y),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.6,
            (0, 255, 0),
            2,
            cv2.LINE_AA,
        )
        y += 26

    return bgr


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Connect to HelloAR gRPC stream and display overlays."
    )
    parser.add_argument("--host", default="127.0.0.1", help="Phone IP address")
    parser.add_argument("--port", type=int, default=50051, help="gRPC port")
    parser.add_argument(
        "--da3-model",
        default="python/models/da3_small_2_392x224_sim.onnx",
        help="Path to DA3 ONNX model.",
    )
    parser.add_argument("--da3-width", type=int, default=392, help="DA3 input width.")
    parser.add_argument("--da3-height", type=int, default=224, help="DA3 input height.")
    parser.add_argument(
        "--keyframe-rot-deg",
        type=float,
        default=12.0,
        help="Rotation threshold (deg) to trigger a keyframe.",
    )
    parser.add_argument(
        "--keyframe-trans-m",
        type=float,
        default=0.2,
        help="Translation threshold (meters) to trigger a keyframe.",
    )
    args = parser.parse_args()

    target = f"{args.host}:{args.port}"
    print(f"Connecting to {target}")

    state: Dict[str, Any] = {"count": 0, "started_at": time.time(), "last_log_count": 0}
    window_name = "VLP Gray Stream"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window_name, 1280, 480)

    da3: Optional[DA3PairProcessor] = None
    if not DA3PairProcessor.is_available():
        print("[DA3] onnxruntime not installed, DA3 processor disabled.")
    else:
        try:
            if not os.path.isfile(args.da3_model):
                raise RuntimeError(f"Model file not found: {args.da3_model}")
            da3 = DA3PairProcessor(args.da3_model, args.da3_width, args.da3_height)
        except Exception as e:
            print(f"[DA3] disabled: {e}", file=sys.stderr)
            da3 = None

    keyframes: list[Keyframe] = []
    latest_depth_vis: Optional[np.ndarray] = None
    latest_scale_text = "scale: n/a (pose-translation)"

    channel = grpc.insecure_channel(target)
    stream_frames = channel.unary_stream(
        "/vlp.FrameStreamService/StreamFrames",
        request_serializer=lambda x: x,
        response_deserializer=lambda x: x,
    )

    try:
        print("Connected.")
        for payload in stream_frames(b"subscribe"):
            if len(payload) < FRAME_HEADER_SIZE:
                continue
            (
                magic,
                timestamp_ns,
                width,
                height,
                fx,
                fy,
                cx,
                cy,
                qx,
                qy,
                qz,
                qw,
                tx,
                ty,
                tz,
            ) = FRAME_HEADER_STRUCT.unpack_from(payload, 0)
            if magic != FRAME_MAGIC:
                continue

            jpeg_bytes = payload[FRAME_HEADER_SIZE:]
            if len(jpeg_bytes) == 0:
                continue
            jpg_array = np.frombuffer(jpeg_bytes, dtype=np.uint8)
            image_bgr = cv2.imdecode(jpg_array, cv2.IMREAD_COLOR)
            if image_bgr is None:
                continue

            intrinsics = {"fx": fx, "fy": fy, "cx": cx, "cy": cy}
            pose = {"qx": qx, "qy": qy, "qz": qz, "qw": qw, "tx": tx, "ty": ty, "tz": tz}
            frame_pose = FramePose(qx=qx, qy=qy, qz=qz, qw=qw, tx=tx, ty=ty, tz=tz)

            state["count"] += 1
            frame_idx = state["count"]
            elapsed = time.time() - state["started_at"]
            fps = (frame_idx / elapsed) if elapsed > 0 else 0.0

            is_keyframe = False
            if not keyframes:
                is_keyframe = True
            else:
                last_kf = keyframes[-1]
                rot_deg = _quat_angle_deg(last_kf.pose, frame_pose)
                trans_m = _trans_delta(last_kf.pose, frame_pose)
                is_keyframe = rot_deg >= args.keyframe_rot_deg or trans_m >= args.keyframe_trans_m

            if is_keyframe:
                kf = Keyframe(
                    idx=len(keyframes) + 1,
                    timestamp_ns=timestamp_ns,
                    image_bgr=image_bgr.copy(),
                    pose=frame_pose,
                )
                keyframes.append(kf)
                print(f"[KF] kf{kf.idx} ts={timestamp_ns}")
                if da3 is not None and len(keyframes) >= 2:
                    outputs = da3.infer_pair(
                        keyframes[-2].image_bgr,
                        keyframes[-1].image_bgr,
                        pair_label=f"(kf{keyframes[-2].idx}, kf{keyframes[-1].idx})",
                    )
                    depth_rel = None
                    if outputs is not None and "depth" in outputs:
                        depth_tensor = outputs["depth"]  # expected [1, 2, H, W]
                        if depth_tensor.ndim == 4 and depth_tensor.shape[1] >= 2:
                            depth_rel = depth_tensor[0, 1].astype(np.float32)
                        else:
                            depth_rel = DA3PairProcessor.output_to_depth_map(depth_tensor)
                    if depth_rel is not None:
                        depth_rel = cv2.resize(
                            depth_rel,
                            (image_bgr.shape[1], image_bgr.shape[0]),
                            interpolation=cv2.INTER_LINEAR,
                        )
                        scale = (
                            _scale_from_translation_delta(
                                outputs, keyframes[-2].pose, keyframes[-1].pose
                            )
                            if outputs is not None
                            else None
                        )
                        if scale is not None and np.isfinite(scale):
                            depth_scaled = depth_rel * float(scale)
                            vis = DA3PairProcessor.output_to_vis(depth_scaled)
                            latest_scale_text = f"scale={scale:.4f} (real_t/da3_t)"
                        else:
                            vis = DA3PairProcessor.output_to_vis(depth_rel)
                            latest_scale_text = "scale: failed (translation)"
                        if vis is not None:
                            latest_depth_vis = cv2.resize(
                                vis,
                                (image_bgr.shape[1], image_bgr.shape[0]),
                                interpolation=cv2.INTER_LINEAR,
                            )

            overlay = draw_overlay(image_bgr, timestamp_ns, intrinsics, pose, fps)
            if is_keyframe:
                cv2.putText(
                    overlay,
                    "KEYFRAME",
                    (10, overlay.shape[0] - 16),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.8,
                    (0, 0, 255),
                    2,
                    cv2.LINE_AA,
                )
            if latest_depth_vis is None:
                depth_panel = np.zeros_like(overlay)
                cv2.putText(
                    depth_panel,
                    "No depth yet",
                    (20, 40),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    1.0,
                    (255, 255, 255),
                    2,
                    cv2.LINE_AA,
                )
            else:
                depth_panel = latest_depth_vis
            cv2.putText(
                depth_panel,
                latest_scale_text,
                (10, 24),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.6,
                (255, 255, 255),
                2,
                cv2.LINE_AA,
            )
            combined = np.hstack([overlay, depth_panel])
            cv2.imshow(window_name, combined)
            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                break

            if frame_idx - state["last_log_count"] >= 30:
                state["last_log_count"] = frame_idx
                print(
                    f"[{frame_idx}] stream_fps={fps:.2f} ts={timestamp_ns} "
                    f"size={width}x{height}"
                )
        elapsed = time.time() - state["started_at"]
        print(
            f"Closed. frames={state['count']} elapsed={elapsed:.2f}s "
            f"fps={(state['count'] / elapsed) if elapsed > 0 else 0:.2f}"
        )
    except grpc.RpcError as e:
        print(f"gRPC error: {e}", file=sys.stderr)
    finally:
        channel.close()
        cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
