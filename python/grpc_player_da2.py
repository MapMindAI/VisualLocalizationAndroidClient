#!/usr/bin/env python3
import argparse
import asyncio
import base64
import collections
import json
import http
import struct
import threading
import time
from typing import Deque, Iterator, List, Optional, Set, Tuple

import cv2
import numpy as np
import onnxruntime as ort
import websockets
from websockets.datastructures import Headers
from websockets.http11 import Response

from models.test_da2_onnx import resolve_hw, to_depth_vis

FILE_MAGIC = b"VLPREC1\n"
FILE_VERSION = 1
FILE_HEADER_STRUCT = struct.Struct("<8sI")
ENTRY_HEADER_STRUCT = struct.Struct("<QI")

FRAME_MAGIC = 0x564C5032  # "VLP2"
FRAME_HEADER_STRUCT = struct.Struct("<IQII11f")
FRAME_HEADER_SIZE = FRAME_HEADER_STRUCT.size


class _WebPublisher:
    def __init__(self, port: int, html_path: str):
        self._port = port
        self._html_path = html_path
        self._clients: Set[websockets.WebSocketServerProtocol] = set()
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None
        self._started = threading.Event()
        self._stopped = False
        self._html_bytes = b""
        self._html_ok = False
        try:
            with open(html_path, "rb") as f:
                self._html_bytes = f.read()
                self._html_ok = True
        except Exception:
            self._html_bytes = b"web_client.html not found\n"
            self._html_ok = False

    async def _ws_handler(self, ws: websockets.WebSocketServerProtocol):
        self._clients.add(ws)
        try:
            async for _ in ws:
                pass
        finally:
            self._clients.discard(ws)

    async def _process_request(self, _conn, request):
        path = request.path
        if path == "/ws":
            return None

        if path == "/" or path == "/index.html":
            status = http.HTTPStatus.OK if self._html_ok else http.HTTPStatus.INTERNAL_SERVER_ERROR
            reason = "OK" if self._html_ok else "Internal Server Error"
            headers = Headers()
            headers["Content-Type"] = "text/html; charset=utf-8"
            headers["Content-Length"] = str(len(self._html_bytes))
            headers["Connection"] = "close"
            return Response(status, reason, headers, self._html_bytes)

        body = b"Not Found\n"
        headers = Headers()
        headers["Content-Type"] = "text/plain; charset=utf-8"
        headers["Content-Length"] = str(len(body))
        headers["Connection"] = "close"
        return Response(http.HTTPStatus.NOT_FOUND, "Not Found", headers, body)

    async def _run(self):
        async with websockets.serve(
            self._ws_handler, "0.0.0.0", self._port, process_request=self._process_request
        ):
            self._started.set()
            while not self._stopped:
                await asyncio.sleep(0.05)

    def start(self):
        def _thread_main():
            self._loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self._loop)
            self._loop.run_until_complete(self._run())

        self._thread = threading.Thread(target=_thread_main, daemon=True)
        self._thread.start()
        self._started.wait(timeout=3.0)

    async def _broadcast_async(self, payload: str):
        if not self._clients:
            return
        dead = []
        for ws in list(self._clients):
            try:
                await ws.send(payload)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self._clients.discard(ws)

    def broadcast_json(self, obj: dict):
        if self._loop is None:
            return
        payload = json.dumps(obj, separators=(",", ":"))
        asyncio.run_coroutine_threadsafe(self._broadcast_async(payload), self._loop)

    def stop(self):
        self._stopped = True
        if self._loop is not None:
            self._loop.call_soon_threadsafe(lambda: None)
        if self._thread is not None:
            self._thread.join(timeout=1.0)


def _iter_entries(path: str) -> Iterator[Tuple[int, bytes]]:
    with open(path, "rb") as f:
        header = f.read(FILE_HEADER_STRUCT.size)
        if len(header) != FILE_HEADER_STRUCT.size:
            raise RuntimeError("recording file too short")
        magic, version = FILE_HEADER_STRUCT.unpack(header)
        if magic != FILE_MAGIC:
            raise RuntimeError("invalid recording magic")
        if version != FILE_VERSION:
            raise RuntimeError(f"unsupported recording version: {version}")

        while True:
            entry = f.read(ENTRY_HEADER_STRUCT.size)
            if not entry:
                break
            if len(entry) != ENTRY_HEADER_STRUCT.size:
                raise RuntimeError("truncated entry header")
            rel_ns, payload_len = ENTRY_HEADER_STRUCT.unpack(entry)
            payload = f.read(payload_len)
            if len(payload) != payload_len:
                raise RuntimeError("truncated payload")
            yield rel_ns, payload


def _load_da2(model: str, providers: str, fallback_h: int, fallback_w: int):
    providers_list = [p.strip() for p in providers.split(",") if p.strip()]
    session = ort.InferenceSession(model, providers=providers_list)
    inputs_meta = {inp.name: inp for inp in session.get_inputs()}
    outputs_meta = [out.name for out in session.get_outputs()]
    if "image" not in inputs_meta:
        raise ValueError(f"Model input 'image' not found. Inputs: {list(inputs_meta.keys())}")
    if "depth" not in outputs_meta:
        raise ValueError(f"Model output 'depth' not found. Outputs: {outputs_meta}")
    h, w = resolve_hw(inputs_meta["image"].shape, fallback_h, fallback_w)
    return session, h, w


def _run_da2(session: ort.InferenceSession, bgr: np.ndarray, w: int, h: int) -> np.ndarray:
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    resized = cv2.resize(rgb, (w, h), interpolation=cv2.INTER_LINEAR)
    image = np.transpose(resized.astype(np.float32) / 255.0, (2, 0, 1))[None, ...]
    depth = session.run(["depth"], {"image": image})[0][0]
    return depth


def _quat_to_rot(qx: float, qy: float, qz: float, qw: float) -> np.ndarray:
    n = qx * qx + qy * qy + qz * qz + qw * qw
    if n < 1e-12:
        return np.eye(3, dtype=np.float32)
    s = 2.0 / n
    xx, yy, zz = qx * qx * s, qy * qy * s, qz * qz * s
    xy, xz, yz = qx * qy * s, qx * qz * s, qy * qz * s
    wx, wy, wz = qw * qx * s, qw * qy * s, qw * qz * s
    return np.array(
        [
            [1.0 - (yy + zz), xy - wz, xz + wy],
            [xy + wz, 1.0 - (xx + zz), yz - wx],
            [xz - wy, yz + wx, 1.0 - (xx + yy)],
        ],
        dtype=np.float32,
    )


def _quat_mul(
    qx1: float, qy1: float, qz1: float, qw1: float, qx2: float, qy2: float, qz2: float, qw2: float
) -> Tuple[float, float, float, float]:
    # Hamilton product q = q1 * q2 for (x, y, z, w) layout.
    w = qw1 * qw2 - qx1 * qx2 - qy1 * qy2 - qz1 * qz2
    x = qw1 * qx2 + qx1 * qw2 + qy1 * qz2 - qz1 * qy2
    y = qw1 * qy2 - qx1 * qz2 + qy1 * qw2 + qz1 * qx2
    z = qw1 * qz2 + qx1 * qy2 - qy1 * qx2 + qz1 * qw2
    return x, y, z, w


def _apply_camera_to_opengl_pose(
    qx: float, qy: float, qz: float, qw: float, tx: float, ty: float, tz: float
) -> Tuple[float, float, float, float, float, float, float]:
    # Match C++:
    #   const Sophus::SE3f kTransformCameraToOpenGLDevice(
    #       Eigen::Quaternionf(0.0f, 1.0f, 0.0f, 0.0f), Eigen::Vector3f::Zero());
    #   packet.pose = packet.pose * kTransformCameraToOpenGLDevice;
    #
    # Right-multiply by fixed rotation 180deg around X.
    q2x, q2y, q2z, q2w = 1.0, 0.0, 0.0, 0.0
    nx, ny, nz, nw = _quat_mul(qx, qy, qz, qw, q2x, q2y, q2z, q2w)
    return nx, ny, nz, nw, tx, ty, tz


def _make_cloud_frame(
    depth: np.ndarray,
    bgr: np.ndarray,
    fx: float,
    fy: float,
    cx: float,
    cy: float,
    qx: float,
    qy: float,
    qz: float,
    qw: float,
    tx: float,
    ty: float,
    tz: float,
    depth_max: float,
    px_step: int,
) -> np.ndarray:
    h, w = depth.shape
    step = max(1, px_step)
    uu = np.arange(0, w, step, dtype=np.float32)
    vv = np.arange(0, h, step, dtype=np.float32)
    grid_u, grid_v = np.meshgrid(uu, vv)

    d = depth[::step, ::step].astype(np.float32)
    # d = (25.0 - d) * 0.1


    valid = np.isfinite(d) & (d > 1e-4) & (d < depth_max)
    if not np.any(valid):
        return np.zeros((0, 6), dtype=np.float32)

    z = d[valid]
    u = grid_u[valid]
    v = grid_v[valid]
    x = (u - cx) * z / max(fx, 1e-6)
    y = (v - cy) * z / max(fy, 1e-6)
    pc = np.stack([x, y, z], axis=1)

    rot = _quat_to_rot(qx, qy, qz, qw)
    t = np.array([tx, ty, tz], dtype=np.float32)
    pw = (pc @ rot.T) + t[None, :]

    bgr_small = bgr[::step, ::step]
    c = bgr_small[valid]
    rgb = np.stack([c[:, 2], c[:, 1], c[:, 0]], axis=1).astype(np.float32)
    return np.concatenate([pw, rgb], axis=1)


def _pack_cloud_recent(frames: Deque[np.ndarray], max_points: int) -> List[List[float]]:
    if not frames:
        return []
    cloud = np.concatenate(list(frames), axis=0) if len(frames) > 1 else frames[0]
    if cloud.shape[0] > max_points:
        idx = np.linspace(0, cloud.shape[0] - 1, max_points, dtype=np.int32)
        cloud = cloud[idx]
    out = []
    for p in cloud:
        out.append([float(p[0]), float(p[1]), float(p[2]), float(p[3]), float(p[4]), float(p[5])])
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Replay recorded gRPC stream + DA2 depth side-by-side.")
    parser.add_argument("--input", required=True, help="Recorded file path")
    parser.add_argument("--model", default="python/models/depth_anything_v2_vits.onnx", help="DA2 ONNX model path")
    parser.add_argument("--providers", default="CPUExecutionProvider", help="Comma-separated ORT providers")
    parser.add_argument("--height", type=int, default=518, help="Fallback model input height")
    parser.add_argument("--width", type=int, default=518, help="Fallback model input width")
    parser.add_argument("--speed", type=float, default=1.0, help="Playback speed (e.g. 2.0)")
    parser.add_argument("--loop", action="store_true", help="Loop playback")
    parser.add_argument("--no-display", action="store_true", help="Disable OpenCV display")
    parser.add_argument("--websocket-port", type=int, default=9002, help="Websocket+HTTP port for html viewer")
    parser.add_argument("--web-client-html", type=str, default="mapping/backend/web_client.html", help="HTML viewer path")
    parser.add_argument("--pc-recent-n", type=int, default=8, help="Number of recent depth frames for cloud fusion")
    parser.add_argument("--pc-pixel-step", type=int, default=8, help="Pixel step for depth backprojection")
    parser.add_argument("--pc-max-points", type=int, default=30000, help="Max cloud points sent to web")
    parser.add_argument("--depth-max", type=float, default=10.0, help="Max depth in meters for point cloud")
    args = parser.parse_args()

    if args.speed <= 0:
        raise ValueError("--speed must be > 0")

    session, in_h, in_w = _load_da2(args.model, args.providers, args.height, args.width)
    web = _WebPublisher(args.websocket_port, args.web_client_html)
    web.start()
    print(f"Open web client: http://127.0.0.1:{args.websocket_port}/index.html")

    window_name = "VLP Playback + DA2"
    if not args.no_display:
        cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(window_name, 1400, 700)

    recent_clouds: Deque[np.ndarray] = collections.deque(maxlen=max(1, args.pc_recent_n))
    trajectory: Deque[List[float]] = collections.deque(maxlen=1200)
    while True:
        played = 0
        playback_start_ns = time.monotonic_ns()
        first_rel_ns = None
        for rel_ns, payload in _iter_entries(args.input):
            if first_rel_ns is None:
                first_rel_ns = rel_ns
            target_rel_ns = int((rel_ns - first_rel_ns) / args.speed)
            while True:
                elapsed_ns = time.monotonic_ns() - playback_start_ns
                remain_ns = target_rel_ns - elapsed_ns
                if remain_ns <= 0:
                    break
                time.sleep(min(0.002, remain_ns / 1e9))

            if len(payload) < FRAME_HEADER_SIZE:
                continue
            unpacked = FRAME_HEADER_STRUCT.unpack_from(payload, 0)
            magic = unpacked[0]
            if magic != FRAME_MAGIC:
                continue
            timestamp_ns = unpacked[1]
            fx, fy, cx, cy = float(unpacked[4]), float(unpacked[5]), float(unpacked[6]), float(unpacked[7])
            qx, qy, qz, qw = float(unpacked[8]), float(unpacked[9]), float(unpacked[10]), float(unpacked[11])
            tx, ty, tz = float(unpacked[12]), float(unpacked[13]), float(unpacked[14])
            qx, qy, qz, qw, tx, ty, tz = _apply_camera_to_opengl_pose(qx, qy, qz, qw, tx, ty, tz)
            jpeg_bytes = payload[FRAME_HEADER_SIZE:]
            if not jpeg_bytes:
                continue

            img = cv2.imdecode(np.frombuffer(jpeg_bytes, dtype=np.uint8), cv2.IMREAD_COLOR)
            if img is None:
                continue

            t0 = time.perf_counter()
            depth = _run_da2(session, img, in_w, in_h)
            infer_ms = (time.perf_counter() - t0) * 1000.0

            depth_vis = to_depth_vis(depth)
            depth_vis = cv2.resize(depth_vis, (img.shape[1], img.shape[0]), interpolation=cv2.INTER_LINEAR)
            panel = np.concatenate([img, depth_vis], axis=0)
            depth_full = cv2.resize(depth.astype(np.float32), (img.shape[1], img.shape[0]), interpolation=cv2.INTER_LINEAR)
            cloud_frame = _make_cloud_frame(
                depth_full, img, fx, fy, cx, cy, qx, qy, qz, qw, tx, ty, tz,
                args.depth_max, args.pc_pixel_step
            )
            recent_clouds.append(cloud_frame)
            cloud_points = _pack_cloud_recent(recent_clouds, max(1000, args.pc_max_points))
            depth_min = float(np.min(depth_full)) if depth_full.size else 0.0
            depth_max = float(np.max(depth_full)) if depth_full.size else 0.0
            trajectory.append([tx, ty, tz])

            cv2.putText(
                panel,
                f"play_ts(ns): {timestamp_ns} speed: {args.speed:.2f}x da2: {infer_ms:.1f} ms",
                (10, 28),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.7,
                (0, 255, 0),
                2,
                cv2.LINE_AA,
            )

            if not args.no_display:
                cv2.imshow(window_name, panel)
                if (cv2.waitKey(1) & 0xFF) == ord("q"):
                    cv2.destroyAllWindows()
                    return 0

            played += 1
            if played % 20 == 0:
                print(f"[{played}] timestamp_ns={timestamp_ns} da2_ms={infer_ms:.1f}")

            ok, enc = cv2.imencode(".jpg", panel, [int(cv2.IMWRITE_JPEG_QUALITY), 80])
            if ok:
                img_b64 = base64.b64encode(enc.tobytes()).decode("ascii")
                web.broadcast_json(
                    {
                        "type": "update",
                        "frame_id": played,
                        "image_jpeg_b64": img_b64,
                        "fps": 1000.0 / max(infer_ms, 1e-6),
                        "pose": {"tx": tx, "ty": ty, "tz": tz},
                        "da3_status": f"DA2 infer={infer_ms:.1f}ms",
                        "voxblox": {
                            "enabled": False,
                            "integrated_frames": 0,
                            "voxel_size_m": 0.05,
                            "esdf_count": 0,
                        },
                        "trajectory": list(trajectory),
                        "depth_clouds": cloud_points,
                        "depth_cloud_count": len(cloud_points),
                        "depth_range": [depth_min, depth_max],
                    }
                )

        print(f"Playback finished. Frames: {played}")
        if not args.loop:
            break
        print("Looping...")

    if not args.no_display:
        cv2.destroyAllWindows()
    web.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
