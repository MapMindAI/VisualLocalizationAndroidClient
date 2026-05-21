#!/usr/bin/env python3
import argparse
import asyncio
import base64
import collections
import http
import json
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

FRAME_MAGIC_VLP2 = 0x564C5032
FRAME_MAGIC_VLP3 = 0x564C5033
FRAME_HEADER_STRUCT = struct.Struct("<IQII11f")
FRAME_HEADER_SIZE = FRAME_HEADER_STRUCT.size  # 64
EXT_HEADER_STRUCT = struct.Struct("<IIIII")
DEPTH_FMT_U16_MM = 1

FILE_MAGIC = b"VLPREC1\n"
FILE_VERSION = 1
FILE_HEADER_STRUCT = struct.Struct("<8sI")
ENTRY_HEADER_STRUCT = struct.Struct("<QI")


def load_da2(model_path: str, providers: str, fallback_h: int, fallback_w: int):
    provider_list = [p.strip() for p in providers.split(",") if p.strip()]
    session = ort.InferenceSession(model_path, providers=provider_list)
    inputs_meta = {inp.name: inp for inp in session.get_inputs()}
    if "image" not in inputs_meta:
        raise ValueError(f"Model input 'image' not found. Inputs: {list(inputs_meta.keys())}")
    h, w = resolve_hw(inputs_meta["image"].shape, fallback_h, fallback_w)
    return session, h, w


def run_da2_depth(session: ort.InferenceSession, bgr: np.ndarray, in_w: int, in_h: int) -> np.ndarray:
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    resized = cv2.resize(rgb, (in_w, in_h), interpolation=cv2.INTER_LINEAR)
    image = np.transpose(resized.astype(np.float32) / 255.0, (2, 0, 1))[None, ...]
    depth = session.run(["depth"], {"image": image})[0][0]
    return depth.astype(np.float32)


class WebPublisher:
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


def iter_record_entries(path: str) -> Iterator[Tuple[int, bytes]]:
    with open(path, "rb") as f:
        header = f.read(FILE_HEADER_STRUCT.size)
        if len(header) != FILE_HEADER_STRUCT.size:
            raise ValueError(f"record file too small: {path}")
        magic, version = FILE_HEADER_STRUCT.unpack(header)
        if magic != FILE_MAGIC:
            raise ValueError(f"invalid record magic in {path}: {magic!r}")
        if version != FILE_VERSION:
            raise ValueError(f"unsupported record version {version} (expected {FILE_VERSION})")

        while True:
            eh = f.read(ENTRY_HEADER_STRUCT.size)
            if not eh:
                break
            if len(eh) != ENTRY_HEADER_STRUCT.size:
                raise ValueError("truncated entry header")
            rel_ns, payload_len = ENTRY_HEADER_STRUCT.unpack(eh)
            payload = f.read(payload_len)
            if len(payload) != payload_len:
                raise ValueError("truncated entry payload")
            yield int(rel_ns), payload


def quat_mul(q1: Tuple[float, float, float, float], q2: Tuple[float, float, float, float]):
    x1, y1, z1, w1 = q1
    x2, y2, z2, w2 = q2
    w = w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2
    x = w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2
    y = w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2
    z = w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2
    return x, y, z, w


def quat_to_rot(qx: float, qy: float, qz: float, qw: float) -> np.ndarray:
    n = qx * qx + qy * qy + qz * qz + qw * qw
    if n < 1e-12:
        return np.eye(3, dtype=np.float32)
    s = 2.0 / n
    xx, yy, zz = qx * qx * s, qy * qy * s, qz * qz * s
    xy, xz, yz = qx * qy * s, qx * qz * s, qy * qz * s
    wx, wy, wz = qw * qx * s, qw * qy * s, qw * qz * s
    return np.array([
        [1.0 - (yy + zz), xy - wz, xz + wy],
        [xy + wz, 1.0 - (xx + zz), yz - wx],
        [xz - wy, yz + wx, 1.0 - (xx + yy)],
    ], dtype=np.float32)


def apply_camera_to_opengl(qx: float, qy: float, qz: float, qw: float):
    return quat_mul((qx, qy, qz, qw), (1.0, 0.0, 0.0, 0.0))


def make_cloud_frame(
    depth_m: np.ndarray,
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
    h, w = depth_m.shape
    step = max(1, px_step)
    uu = np.arange(0, w, step, dtype=np.float32)
    vv = np.arange(0, h, step, dtype=np.float32)
    gu, gv = np.meshgrid(uu, vv)
    d = depth_m[::step, ::step]
    valid = np.isfinite(d) & (d > 1e-4) & (d < depth_max)
    if not np.any(valid):
        return np.zeros((0, 6), dtype=np.float32)
    z = d[valid]
    u = gu[valid]
    v = gv[valid]
    x = (u - cx) * z / max(fx, 1e-6)
    y = (v - cy) * z / max(fy, 1e-6)
    pc = np.stack([x, y, z], axis=1)
    rot = quat_to_rot(qx, qy, qz, qw)
    t = np.array([tx, ty, tz], dtype=np.float32)
    pw = (pc @ rot.T) + t[None, :]
    c = bgr[::step, ::step][valid]
    rgb = np.stack([c[:, 2], c[:, 1], c[:, 0]], axis=1).astype(np.float32)
    return np.concatenate([pw, rgb], axis=1)


def pack_recent_cloud(frames: Deque[np.ndarray], max_points: int) -> List[List[float]]:
    if not frames:
        return []
    cloud = np.concatenate(list(frames), axis=0) if len(frames) > 1 else frames[0]
    if cloud.shape[0] > max_points:
        idx = np.linspace(0, cloud.shape[0] - 1, max_points, dtype=np.int32)
        cloud = cloud[idx]
    return [[float(p[0]), float(p[1]), float(p[2]), float(p[3]), float(p[4]), float(p[5])] for p in cloud]


def parse_payload(payload: bytes):
    if len(payload) < FRAME_HEADER_SIZE:
        return None
    unpacked = FRAME_HEADER_STRUCT.unpack_from(payload, 0)
    magic = unpacked[0]
    ts = int(unpacked[1])
    width, height = int(unpacked[2]), int(unpacked[3])
    fx, fy, cx, cy = float(unpacked[4]), float(unpacked[5]), float(unpacked[6]), float(unpacked[7])
    qx, qy, qz, qw = float(unpacked[8]), float(unpacked[9]), float(unpacked[10]), float(unpacked[11])
    tx, ty, tz = float(unpacked[12]), float(unpacked[13]), float(unpacked[14])

    if magic == FRAME_MAGIC_VLP2:
        jpeg = payload[FRAME_HEADER_SIZE:]
        return {
            "magic": magic,
            "timestamp_ns": ts,
            "width": width,
            "height": height,
            "fx": fx,
            "fy": fy,
            "cx": cx,
            "cy": cy,
            "qx": qx,
            "qy": qy,
            "qz": qz,
            "qw": qw,
            "tx": tx,
            "ty": ty,
            "tz": tz,
            "jpeg": jpeg,
            "depth": None,
        }

    if magic == FRAME_MAGIC_VLP3:
        if len(payload) < FRAME_HEADER_SIZE + EXT_HEADER_STRUCT.size:
            return None
        jpeg_len, dw, dh, dformat, dlen = EXT_HEADER_STRUCT.unpack_from(payload, FRAME_HEADER_SIZE)
        off = FRAME_HEADER_SIZE + EXT_HEADER_STRUCT.size
        if off + jpeg_len > len(payload):
            return None
        jpeg = payload[off : off + jpeg_len]
        depth = None
        if dformat == DEPTH_FMT_U16_MM and dlen > 0 and off + jpeg_len + dlen <= len(payload):
            db = payload[off + jpeg_len : off + jpeg_len + dlen]
            arr = np.frombuffer(db, dtype="<u2")
            if dw > 0 and dh > 0 and arr.size >= dw * dh:
                depth = arr[: dw * dh].reshape((dh, dw)).astype(np.float32) * 0.001
        return {
            "magic": magic,
            "timestamp_ns": ts,
            "width": width,
            "height": height,
            "fx": fx,
            "fy": fy,
            "cx": cx,
            "cy": cy,
            "qx": qx,
            "qy": qy,
            "qz": qz,
            "qw": qw,
            "tx": tx,
            "ty": ty,
            "tz": tz,
            "jpeg": jpeg,
            "depth": depth,
        }

    return None


def run_once(
    args,
    web: WebPublisher,
    window_name: str,
    da2_session: ort.InferenceSession,
    da2_h: int,
    da2_w: int,
    video_state: Optional[dict],
) -> bool:
    recent_clouds: Deque[np.ndarray] = collections.deque(maxlen=max(1, args.pc_recent_n))
    trajectory: Deque[List[float]] = collections.deque(maxlen=1200)
    frame_id = 0
    t_last = time.perf_counter()

    replay_t0 = time.perf_counter()
    first_rel_ns: Optional[int] = None

    for rel_ns, payload in iter_record_entries(args.input):
        if first_rel_ns is None:
            first_rel_ns = rel_ns
        rel_from_start_s = float(rel_ns - first_rel_ns) * 1e-9
        speed = max(1e-6, float(args.speed))
        target_s = rel_from_start_s / speed
        now_s = time.perf_counter() - replay_t0
        wait_s = target_s - now_s
        if wait_s > 0.0:
            time.sleep(wait_s)

        p = parse_payload(payload)
        if p is None:
            continue

        img = cv2.imdecode(np.frombuffer(p["jpeg"], dtype=np.uint8), cv2.IMREAD_COLOR)
        if img is None:
            continue

        qx, qy, qz, qw = apply_camera_to_opengl(p["qx"], p["qy"], p["qz"], p["qw"])
        tx, ty, tz = p["tx"], p["ty"], p["tz"]

        t_infer0 = time.perf_counter()
        da2_depth = run_da2_depth(da2_session, img, da2_w, da2_h)
        infer_ms = (time.perf_counter() - t_infer0) * 1000.0

        da2_depth_vis = to_depth_vis(da2_depth)
        da2_depth_vis = cv2.resize(da2_depth_vis, (img.shape[1], img.shape[0]), interpolation=cv2.INTER_LINEAR)
        depth_full = cv2.resize(da2_depth, (img.shape[1], img.shape[0]), interpolation=cv2.INTER_LINEAR)
        depth_full *= max(1e-6, float(args.da2_depth_scale))

        grpc_depth = p["depth"]
        grpc_depth_min = 0.0
        grpc_depth_max = 0.0
        if grpc_depth is not None:
            grpc_depth_full = cv2.resize(grpc_depth, (img.shape[1], img.shape[0]), interpolation=cv2.INTER_LINEAR)
            grpc_depth_vis = to_depth_vis(grpc_depth_full)
            grpc_depth_min = float(np.min(grpc_depth_full))
            grpc_depth_max = float(np.max(grpc_depth_full))
        else:
            grpc_depth_vis = np.zeros_like(img)

        panel = np.concatenate([img, grpc_depth_vis, da2_depth_vis], axis=1)
        cv2.putText(panel, "RGB", (10, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.65, (255, 255, 255), 2, cv2.LINE_AA)
        cv2.putText(
            panel,
            "ARCore depth",
            (img.shape[1] + 10, 24),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.65,
            (255, 255, 255),
            2,
            cv2.LINE_AA,
        )
        cv2.putText(
            panel,
            "DA depth",
            (img.shape[1] * 2 + 10, 24),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.65,
            (255, 255, 255),
            2,
            cv2.LINE_AA,
        )

        cloud_frame = make_cloud_frame(
            depth_full,
            img,
            p["fx"],
            p["fy"],
            p["cx"],
            p["cy"],
            qx,
            qy,
            qz,
            qw,
            tx,
            ty,
            tz,
            args.depth_max,
            args.pc_pixel_step,
        )
        recent_clouds.append(cloud_frame)
        cloud_points = pack_recent_cloud(recent_clouds, max(1000, args.pc_max_points))
        dmin = float(np.min(depth_full))
        dmax = float(np.max(depth_full))

        now = time.perf_counter()
        dt = max(1e-6, now - t_last)
        t_last = now
        fps = 1.0 / dt
        trajectory.append([tx, ty, tz])

        cv2.putText(
            panel,
            f"ts={p['timestamp_ns']} fps={fps:.1f}",
            (10, 28),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            (0, 255, 0),
            2,
            cv2.LINE_AA,
        )
        cv2.putText(
            panel,
            f"da2={infer_ms:.1f}ms magic=0x{p['magic']:08x}",
            (10, 56),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            (0, 255, 0),
            2,
            cv2.LINE_AA,
        )

        if video_state is not None:
            if video_state["writer"] is None:
                h, w = panel.shape[:2]
                fourcc = cv2.VideoWriter_fourcc(*"mp4v")
                writer = cv2.VideoWriter(video_state["path"], fourcc, video_state["fps"], (w, h))
                if not writer.isOpened():
                    raise RuntimeError(f"failed to open mp4 writer: {video_state['path']}")
                video_state["writer"] = writer
            video_state["writer"].write(panel)

        if not args.no_display:
            cv2.imshow(window_name, panel)
            if (cv2.waitKey(1) & 0xFF) == ord("q"):
                return False

        ok, enc = cv2.imencode(".jpg", panel, [int(cv2.IMWRITE_JPEG_QUALITY), 80])
        if ok:
            web.broadcast_json(
                {
                    "type": "update",
                    "frame_id": frame_id,
                    "image_jpeg_b64": base64.b64encode(enc.tobytes()).decode("ascii"),
                    "fps": fps,
                    "pose": {"tx": tx, "ty": ty, "tz": tz},
                    "scale_text": f"DA2 depth x{args.da2_depth_scale:.3f}",
                    "da3_status": (
                        f"arcore_depth=[{grpc_depth_min:.3f},{grpc_depth_max:.3f}] "
                        f"da2_depth=[{dmin:.3f},{dmax:.3f}] infer={infer_ms:.1f}ms"
                    ),
                    "voxblox": {
                        "enabled": False,
                        "integrated_frames": 0,
                        "voxel_size_m": 0.05,
                        "esdf_count": 0,
                    },
                    "trajectory": list(trajectory),
                    "depth_clouds": cloud_points,
                    "depth_cloud_count": len(cloud_points),
                }
            )
        frame_id += 1

    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Replay VLP record with DA2 depth visualizer + web point cloud")
    parser.add_argument("--input", required=True, help="Path to vlp_stream_vlp3.rec")
    parser.add_argument("--model", default="python/models/depth_anything_v2_vits.onnx", help="DA2 ONNX model path")
    parser.add_argument("--providers", default="CPUExecutionProvider", help="Comma-separated ORT providers")
    parser.add_argument("--height", type=int, default=518, help="Fallback model input height")
    parser.add_argument("--width", type=int, default=518, help="Fallback model input width")
    parser.add_argument("--da2-depth-scale", type=float, default=1.0, help="Scale applied to DA2 depth for PCL")
    parser.add_argument("--speed", type=float, default=1.0, help="Replay speed factor")
    parser.add_argument("--loop", action="store_true", help="Loop replay")
    parser.add_argument("--no-display", action="store_true")
    parser.add_argument("--websocket-port", type=int, default=9002)
    parser.add_argument("--web-client-html", default="mapping/backend/web_client.html")
    parser.add_argument("--pc-recent-n", type=int, default=8)
    parser.add_argument("--pc-pixel-step", type=int, default=8)
    parser.add_argument("--pc-max-points", type=int, default=30000)
    parser.add_argument("--depth-max", type=float, default=8.0)
    parser.add_argument("--output-mp4", default="", help="Optional output MP4 path for rendered panel")
    parser.add_argument("--mp4-fps", type=float, default=20.0, help="Output MP4 FPS")
    args = parser.parse_args()

    da2_session, da2_h, da2_w = load_da2(args.model, args.providers, args.height, args.width)
    web = WebPublisher(args.websocket_port, args.web_client_html)
    web.start()
    print(f"Open web client: http://127.0.0.1:{args.websocket_port}/index.html")

    window_name = "Replay VLP + DA2 Depth"
    if not args.no_display:
        cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(window_name, 1400, 700)

    video_state = None
    if args.output_mp4:
        video_state = {
            "path": args.output_mp4,
            "fps": max(1.0, float(args.mp4_fps)),
            "writer": None,
        }
        print(f"Recording MP4: {args.output_mp4} @ {video_state['fps']:.1f} fps")

    try:
        while True:
            completed = run_once(args, web, window_name, da2_session, da2_h, da2_w, video_state)
            if not completed or not args.loop:
                break
    finally:
        if video_state is not None and video_state["writer"] is not None:
            video_state["writer"].release()
        web.stop()
        if not args.no_display:
            cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
