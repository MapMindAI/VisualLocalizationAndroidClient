#!/usr/bin/env python3
import argparse
import struct
import sys
import time
from typing import Any, Dict

import cv2
import grpc
import numpy as np


FRAME_MAGIC = 0x564C5032  # "VLP2"
FRAME_HEADER_STRUCT = struct.Struct("<IQII11f")
FRAME_HEADER_SIZE = FRAME_HEADER_STRUCT.size


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
    args = parser.parse_args()

    target = f"{args.host}:{args.port}"
    print(f"Connecting to {target}")

    state: Dict[str, Any] = {"count": 0, "started_at": time.time(), "last_log_count": 0}
    window_name = "VLP Gray Stream"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window_name, 640, 480)

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

            state["count"] += 1
            frame_idx = state["count"]
            elapsed = time.time() - state["started_at"]
            fps = (frame_idx / elapsed) if elapsed > 0 else 0.0

            overlay = draw_overlay(image_bgr, timestamp_ns, intrinsics, pose, fps)
            cv2.imshow(window_name, overlay)
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
