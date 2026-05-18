#!/usr/bin/env python3
import argparse
import base64
import json
import sys
import time
from typing import Any, Dict

import cv2
import numpy as np
import websocket


def draw_overlay(
    gray: bytes,
    width: int,
    height: int,
    timestamp_ns: int,
    intrinsics: Dict[str, Any],
    pose: Dict[str, Any],
    fps: float,
) -> np.ndarray:
    img = np.frombuffer(gray, dtype=np.uint8).reshape((height, width))
    bgr = cv2.cvtColor(img, cv2.COLOR_GRAY2BGR)

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
        description="Connect to HelloAR frame WebSocket and display stream with overlays."
    )
    parser.add_argument("--host", default="127.0.0.1", help="Phone IP address")
    parser.add_argument("--port", type=int, default=8765, help="WebSocket port")
    args = parser.parse_args()

    url = f"ws://{args.host}:{args.port}"
    print(f"Connecting to {url}")

    state: Dict[str, Any] = {"count": 0, "started_at": time.time(), "last_log_count": 0}
    window_name = "VLP Gray Stream"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window_name, 640, 480)

    def on_open(ws: websocket.WebSocketApp) -> None:
        print("Connected.")

    def on_error(ws: websocket.WebSocketApp, error: Any) -> None:
        print(f"WebSocket error: {error}", file=sys.stderr)

    def on_close(
        ws: websocket.WebSocketApp, close_status_code: Any, close_msg: Any
    ) -> None:
        elapsed = time.time() - state["started_at"]
        print(
            f"Closed. frames={state['count']} elapsed={elapsed:.2f}s "
            f"fps={(state['count'] / elapsed) if elapsed > 0 else 0:.2f}"
        )

    def on_message(ws: websocket.WebSocketApp, message: str) -> None:
        payload = json.loads(message)
        if payload.get("type") != "frame":
            return

        width = int(payload["width"])
        height = int(payload["height"])
        timestamp_ns = int(payload["timestamp_ns"])
        intrinsics = payload["intrinsics"]
        pose = payload["pose"]
        gray = base64.b64decode(payload["gray_b64"])

        expected = width * height
        if len(gray) != expected:
            print(
                f"Warning: gray length mismatch, expected={expected}, got={len(gray)}",
                file=sys.stderr,
            )
            return

        state["count"] += 1
        frame_idx = state["count"]
        elapsed = time.time() - state["started_at"]
        fps = (frame_idx / elapsed) if elapsed > 0 else 0.0

        overlay = draw_overlay(gray, width, height, timestamp_ns, intrinsics, pose, fps)
        cv2.imshow(window_name, overlay)
        key = cv2.waitKey(1) & 0xFF
        if key == ord("q"):
            ws.close()
            return

        if frame_idx - state["last_log_count"] >= 30:
            state["last_log_count"] = frame_idx
            print(
                f"[{frame_idx}] stream_fps={fps:.2f} ts={timestamp_ns} "
                f"size={width}x{height}"
            )

    ws = websocket.WebSocketApp(
        url, on_open=on_open, on_message=on_message, on_error=on_error, on_close=on_close
    )
    try:
        ws.run_forever()
    finally:
        cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
