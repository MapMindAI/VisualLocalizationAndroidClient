#!/usr/bin/env python3
import argparse
import threading
import time
from typing import Optional

import cv2  # type: ignore
import numpy as np  # type: ignore
from flask import Flask, Response, jsonify, request

from ws_stream_client import (
    FramePacket,
    decode_jpeg,
    depth_preview,
    parse_packet,
    send_control,
    stream_frames,
)


def draw_overlay(img: np.ndarray, pkt: FramePacket, fps: float) -> None:
    lines = [
        f"ts={pkt.timestamp_ns} fps={fps:.2f}",
        f"size={pkt.width}x{pkt.height} jpeg={len(pkt.jpeg)}",
        f"fx={pkt.fx:.2f} fy={pkt.fy:.2f} cx={pkt.cx:.2f} cy={pkt.cy:.2f}",
        f"q=({pkt.qx:.3f},{pkt.qy:.3f},{pkt.qz:.3f},{pkt.qw:.3f})",
        f"t=({pkt.tx:.3f},{pkt.ty:.3f},{pkt.tz:.3f})",
        f"depth={'yes' if pkt.depth_ts_ns else 'no'} dts={pkt.depth_ts_ns}",
    ]
    y = 24
    for line in lines:
        cv2.putText(
            img,
            line,
            (10, y),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.55,
            (0, 255, 0),
            2,
            cv2.LINE_AA,
        )
        y += 22


class SharedFrame:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.jpeg: Optional[bytes] = None
        self.status = "waiting for gRPC frames..."


def make_status_image(text: str, width: int = 960, height: int = 540) -> bytes:
    img = np.zeros((height, width, 3), dtype=np.uint8)
    cv2.putText(
        img,
        text,
        (20, height // 2),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.8,
        (0, 255, 255),
        2,
        cv2.LINE_AA,
    )
    ok, enc = cv2.imencode(".jpg", img, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
    return enc.tobytes() if ok else b""


def grpc_worker(
    shared: SharedFrame,
    host: str,
    port: int,
    timeout_s: float,
    show_depth: bool,
) -> None:
    latest_depth_img = None
    last_t = time.time()
    fps = 0.0
    while True:
        try:
            with shared.lock:
                shared.status = f"connecting grpc {host}:{port}..."
            for payload in stream_frames(host, port, timeout_s):
                pkt = parse_packet(payload)
                rgb = decode_jpeg(pkt.jpeg)
                if rgb is None:
                    continue

                now = time.time()
                dt = max(1e-6, now - last_t)
                fps = 0.9 * fps + 0.1 * (1.0 / dt) if fps > 0 else (1.0 / dt)
                last_t = now

                frame = rgb
                if show_depth:
                    depth_img = None
                    if pkt.depth_ts_ns:
                        depth_img = depth_preview(pkt.depth_bytes, pkt.depth_w, pkt.depth_h)
                    if depth_img is not None:
                        depth_img = cv2.resize(
                            depth_img, (rgb.shape[1], rgb.shape[0]), interpolation=cv2.INTER_NEAREST
                        )
                        latest_depth_img = depth_img
                    elif latest_depth_img is not None:
                        depth_img = latest_depth_img
                    else:
                        depth_img = np.zeros_like(rgb)
                    cv2.putText(
                        depth_img,
                        "depth",
                        (10, 24),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.7,
                        (255, 255, 255),
                        2,
                        cv2.LINE_AA,
                    )
                    frame = np.hstack([rgb, depth_img])

                draw_overlay(frame, pkt, fps)
                ok, enc = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), 80])
                if not ok:
                    continue
                with shared.lock:
                    shared.jpeg = enc.tobytes()
                    shared.status = f"streaming {pkt.width}x{pkt.height} fps={fps:.2f}"
        except Exception as e:
            with shared.lock:
                shared.status = f"grpc error: {e}"
            time.sleep(1.0)


def create_app(shared: SharedFrame, grpc_host: str, control_grpc_port: int) -> Flask:
    app = Flask(__name__)

    @app.route("/")
    def index() -> str:
        return """
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <title>MapMind gRPC Stream Viewer</title>
  <style>
    body { margin: 0; background: #111; color: #eee; font-family: sans-serif; }
    .wrap { padding: 12px; }
    #status { margin: 0 0 10px 0; color: #9f9; }
    #robot { margin: 10px 0; padding: 10px; border: 1px solid #333; max-width: 700px; }
    #robot input { margin: 4px 6px 4px 0; padding: 4px; }
    #robot button { margin: 4px; padding: 8px 14px; }
    #robotLog { color: #9cf; min-height: 20px; }
    img { width: 100%; max-width: 1400px; border: 1px solid #333; }
  </style>
</head>
<body>
  <div class="wrap">
    <h3>MapMind gRPC Stream Viewer</h3>
    <div id="status">connecting...</div>
    <div id="robot">
      <div><b>Robot Control (gRPC)</b></div>
      <div>
        <button id="btnFront">Front</button>
      </div>
      <div>
        <button id="btnLeft">Left</button>
        <button id="btnStop">Stop</button>
        <button id="btnRight">Right</button>
      </div>
      <div>
        <button id="btnBack">Back</button>
      </div>
      <div id="robotLog">Ready</div>
    </div>
    <img id="v" src="/stream.mjpg" />
  </div>
  <script>
    function robotLog(s) {
      document.getElementById('robotLog').textContent = s;
    }
    async function publishCmd(cmd) {
      try {
        const r = await fetch('/control', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ cmd: cmd })
        });
        const j = await r.json();
        if (!r.ok) {
          robotLog('control error: ' + (j.error || r.status));
          return;
        }
        robotLog('sent: ' + cmd + ' (' + (j.reply || 'ok') + ')');
      } catch (e) {
        robotLog('control request failed: ' + e);
      }
    }
    function bindHold(btnId, moveCmd, stopCmd) {
      const btn = document.getElementById(btnId);
      const start = async (e) => { e.preventDefault(); await publishCmd(moveCmd); };
      const stop = async (e) => { e.preventDefault(); await publishCmd(stopCmd); };
      btn.addEventListener('mousedown', start);
      btn.addEventListener('touchstart', start, { passive: false });
      btn.addEventListener('mouseup', stop);
      btn.addEventListener('mouseleave', stop);
      btn.addEventListener('touchend', stop, { passive: false });
      btn.addEventListener('touchcancel', stop, { passive: false });
    }
    bindHold('btnFront', 8, 5);
    bindHold('btnBack', 2, 5);
    bindHold('btnLeft', 4, 5);
    bindHold('btnRight', 6, 5);
    document.getElementById('btnStop').addEventListener('click', (e) => {
      e.preventDefault();
      publishCmd(5);
    });

    async function pollStatus() {
      try {
        const r = await fetch('/status');
        const t = await r.text();
        document.getElementById('status').textContent = t;
      } catch (e) {}
      setTimeout(pollStatus, 1000);
    }
    pollStatus();
  </script>
</body>
</html>
"""

    @app.route("/status")
    def status() -> str:
        with shared.lock:
            return shared.status

    @app.route("/stream.mjpg")
    def stream_mjpg() -> Response:
        def gen():
            boundary = b"--frame\r\n"
            while True:
                with shared.lock:
                    jpg = shared.jpeg
                    status = shared.status
                if jpg is None:
                    jpg = make_status_image(status)
                yield (
                    boundary
                    + b"Content-Type: image/jpeg\r\n"
                    + f"Content-Length: {len(jpg)}\r\n\r\n".encode("ascii")
                    + jpg
                    + b"\r\n"
                )
                time.sleep(0.05)

        return Response(gen(), mimetype="multipart/x-mixed-replace; boundary=frame")

    @app.route("/control", methods=["POST"])
    def control() -> Response:
      body = request.get_json(silent=True) or {}
      if "cmd" not in body:
        return jsonify({"error": "missing cmd"}), 400
      try:
        cmd = int(body["cmd"])
      except Exception:
        return jsonify({"error": "cmd must be int"}), 400
      try:
        reply = send_control(grpc_host, control_grpc_port, cmd, timeout_s=2.0)
        return jsonify({"ok": True, "reply": reply.decode("utf-8", errors="replace")})
      except Exception as e:
        return jsonify({"error": str(e)}), 500

    return app


def main() -> int:
    ap = argparse.ArgumentParser(description="gRPC to browser MJPEG viewer")
    ap.add_argument("--grpc-host", default="127.0.0.1")
    ap.add_argument("--grpc-port", type=int, default=50051)
    ap.add_argument("--grpc-timeout", type=float, default=3600.0)
    ap.add_argument("--control-grpc-port", type=int, default=50051)
    ap.add_argument("--web-host", default="0.0.0.0")
    ap.add_argument("--web-port", type=int, default=8088)
    ap.add_argument("--show-depth", action="store_true")
    args = ap.parse_args()

    shared = SharedFrame()
    t = threading.Thread(
        target=grpc_worker,
        args=(shared, args.grpc_host, args.grpc_port, args.grpc_timeout, args.show_depth),
        daemon=True,
    )
    t.start()

    app = create_app(shared, args.grpc_host, args.control_grpc_port)
    app.run(host=args.web_host, port=args.web_port, threaded=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
