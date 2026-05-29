#!/usr/bin/env python3
import argparse
import csv
import os
import struct
import sys
import time
from dataclasses import dataclass
from typing import Iterator, Optional


FILE_MAGIC = b"VLPREC1\n"
FILE_VERSION = 1
FILE_HEADER_STRUCT = struct.Struct("<8sI")
ENTRY_HEADER_STRUCT = struct.Struct("<QI")  # rel_ns, payload_size
FRAME_HEADER_STRUCT = struct.Struct("<IQII11f")
FRAME_MAGIC_VLP2 = 0x564C5032
FRAME_MAGIC_VLP3 = 0x564C5033
FRAME_HEADER_SIZE_VLP2 = 64
FRAME_HEADER_SIZE_VLP3 = 84


@dataclass
class FrameRecord:
    index: int
    rel_ns: int
    payload_size: int
    magic_ok: bool
    magic_kind: str
    magic: int
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
    jpeg_size: int
    depth_w: int
    depth_h: int
    depth_size: int
    depth_bytes: bytes
    body: bytes


def _read_exact(f, n: int) -> Optional[bytes]:
    data = f.read(n)
    if not data:
        return None
    if len(data) != n:
        raise ValueError(f"Unexpected EOF: expected {n} bytes, got {len(data)}")
    return data


def _parse_frame_header(payload: bytes):
    if len(payload) < FRAME_HEADER_SIZE_VLP2:
        raise ValueError("Payload too small for frame header")

    magic_raw = struct.unpack_from("<I", payload, 0)[0]

    # Legacy packed 64-byte header. This layout includes tz.
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

    jpeg_offset = FRAME_HEADER_SIZE_VLP2
    jpeg_size, depth_w, depth_h, depth_size = _split_jpeg_and_depth(payload, jpeg_offset)
    kind = "vlp2"
    magic_ok = (magic == FRAME_MAGIC_VLP2)

    # VLP3 extends the header with explicit JPEG/depth metadata.
    if magic == FRAME_MAGIC_VLP3 and len(payload) >= FRAME_HEADER_SIZE_VLP3:
        kind = "vlp3"
        magic_ok = True
        jpeg_offset = FRAME_HEADER_SIZE_VLP3
        jpeg_size = struct.unpack_from("<I", payload, 64)[0]
        depth_w = struct.unpack_from("<I", payload, 68)[0]
        depth_h = struct.unpack_from("<I", payload, 72)[0]
        depth_size = struct.unpack_from("<I", payload, 80)[0]

        # Corrupt metadata fallback: keep parser usable.
        if jpeg_offset + jpeg_size > len(payload):
            jpeg_size = max(0, len(payload) - jpeg_offset)
            depth_size = 0
            depth_w = 0
            depth_h = 0

    return magic_ok, kind, magic, (
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
        jpeg_offset,
        jpeg_size,
        depth_w,
        depth_h,
        depth_size,
    )


def _looks_like_jpeg(data: bytes) -> bool:
    return len(data) >= 4 and data[0:2] == b"\xFF\xD8" and data[-2:] == b"\xFF\xD9"


def _split_jpeg_and_depth(payload: bytes, jpeg_offset: int):
    jpeg_size = max(0, len(payload) - jpeg_offset)
    depth_w = 0
    depth_h = 0
    depth_size = 0
    if jpeg_offset >= len(payload) or payload[jpeg_offset : jpeg_offset + 2] != b"\xFF\xD8":
        return jpeg_size, depth_w, depth_h, depth_size
    eoi = payload.find(b"\xFF\xD9", jpeg_offset + 2)
    if eoi < 0:
        return jpeg_size, depth_w, depth_h, depth_size
    jpeg_size = (eoi + 2) - jpeg_offset
    trailer = payload[eoi + 2 :]
    if len(trailer) >= 24 and trailer[0:4] == b"DPT1":
        depth_w = struct.unpack_from("<I", trailer, 12)[0]
        depth_h = struct.unpack_from("<I", trailer, 16)[0]
        depth_size = struct.unpack_from("<I", trailer, 20)[0]
        if len(trailer) < 24 + depth_size:
            depth_w = 0
            depth_h = 0
            depth_size = 0
    return jpeg_size, depth_w, depth_h, depth_size


def _extract_depth_bytes_vlp2(payload: bytes, jpeg_offset: int, depth_size: int) -> bytes:
    if depth_size <= 0 or jpeg_offset >= len(payload):
        return b""
    eoi = payload.find(b"\xFF\xD9", jpeg_offset + 2)
    if eoi < 0:
        return b""
    trailer = payload[eoi + 2 :]
    if len(trailer) < 24 or trailer[0:4] != b"DPT1":
        return b""
    if len(trailer) < 24 + depth_size:
        return b""
    return trailer[24 : 24 + depth_size]


def _normalize_quat(x: float, y: float, z: float, w: float):
    n2 = x * x + y * y + z * z + w * w
    if n2 <= 1e-20:
        return 0.0, 0.0, 0.0, 1.0
    inv = n2 ** -0.5
    return x * inv, y * inv, z * inv, w * inv


def _quat_mul(ax: float, ay: float, az: float, aw: float, bx: float, by: float, bz: float, bw: float):
    # (x,y,z,w) convention
    x = aw * bx + ax * bw + ay * bz - az * by
    y = aw * by - ax * bz + ay * bw + az * bx
    z = aw * bz + ax * by - ay * bx + az * bw
    w = aw * bw - ax * bx - ay * by - az * bz
    return x, y, z, w


def _apply_camera_to_opengl_pose_transform(
    qx: float, qy: float, qz: float, qw: float, tx: float, ty: float, tz: float
):
    # Matches:
    # Sophus::SE3f pose(q, t);
    # const Sophus::SE3f kTransformCameraToOpenGLDevice(
    #     Eigen::Quaternionf(0.0f, 1.0f, 0.0f, 0.0f), Eigen::Vector3f::Zero());
    # pose = pose * kTransformCameraToOpenGLDevice;
    qx, qy, qz, qw = _normalize_quat(qx, qy, qz, qw)
    # Right multiply by fixed quaternion (x=1, y=0, z=0, w=0).
    qx, qy, qz, qw = _quat_mul(qx, qy, qz, qw, 1.0, 0.0, 0.0, 0.0)
    qx, qy, qz, qw = _normalize_quat(qx, qy, qz, qw)
    # Translation remains unchanged because rhs translation is zero.
    return qx, qy, qz, qw, tx, ty, tz


def iter_vlprec(path: str) -> Iterator[FrameRecord]:
    with open(path, "rb") as f:
        file_header = _read_exact(f, FILE_HEADER_STRUCT.size)
        if file_header is None:
            raise ValueError("Empty file")
        magic, version = FILE_HEADER_STRUCT.unpack(file_header)
        if magic != FILE_MAGIC:
            raise ValueError(f"Bad file magic: {magic!r}")
        if version != FILE_VERSION:
            raise ValueError(f"Unsupported version: {version}, expected {FILE_VERSION}")

        idx = 0
        while True:
            entry_header = f.read(ENTRY_HEADER_STRUCT.size)
            if not entry_header:
                break
            if len(entry_header) != ENTRY_HEADER_STRUCT.size:
                raise ValueError("Corrupt entry header")
            rel_ns, payload_size = ENTRY_HEADER_STRUCT.unpack(entry_header)
            payload = _read_exact(f, payload_size)
            if payload is None:
                raise ValueError("Missing payload bytes")
            if payload_size < FRAME_HEADER_SIZE_VLP2:
                raise ValueError(
                    f"Payload too small at frame {idx}: {payload_size} < {FRAME_HEADER_SIZE_VLP2}"
                )
            magic_ok, magic_kind, frame_magic, fields = _parse_frame_header(payload)
            (
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
                jpeg_offset,
                jpeg_size,
                depth_w,
                depth_h,
                depth_size,
            ) = fields
            body = payload[jpeg_offset : jpeg_offset + jpeg_size]
            depth_bytes = b""
            if magic_kind == "vlp3":
                depth_off = jpeg_offset + jpeg_size
                if depth_size > 0 and depth_off + depth_size <= len(payload):
                    depth_bytes = payload[depth_off : depth_off + depth_size]
            elif magic_kind == "vlp2":
                depth_bytes = _extract_depth_bytes_vlp2(payload, jpeg_offset, depth_size)
            # qx, qy, qz, qw, tx, ty, tz = _apply_camera_to_opengl_pose_transform(
            #     qx, qy, qz, qw, tx, ty, tz
            # )
            yield FrameRecord(
                index=idx,
                rel_ns=rel_ns,
                payload_size=payload_size,
                magic_ok=magic_ok,
                magic_kind=magic_kind,
                magic=frame_magic,
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
                jpeg_size=jpeg_size,
                depth_w=depth_w,
                depth_h=depth_h,
                depth_size=depth_size,
                depth_bytes=depth_bytes,
                body=body,
            )
            idx += 1


def export_body(record: FrameRecord, out_dir: str) -> str:
    os.makedirs(out_dir, exist_ok=True)
    stem = f"frame_{record.index:06d}_ts_{record.timestamp_ns}"
    path = os.path.join(out_dir, f"{stem}.jpg")
    with open(path, "wb") as f:
        f.write(record.body)
    return path


def _decode_jpeg(record: FrameRecord):
    try:
        import cv2  # type: ignore
        import numpy as np  # type: ignore

        arr = np.frombuffer(record.body, dtype=np.uint8)
        return cv2.imdecode(arr, cv2.IMREAD_COLOR)
    except Exception:
        return None


def _draw_overlay(frame_bgr, rec: FrameRecord):
    import cv2  # type: ignore

    lines = [
        f"idx={rec.index} ts={rec.timestamp_ns} rel={rec.rel_ns}",
        f"size={rec.width}x{rec.height} body={len(rec.body)}",
        f"jpeg={rec.jpeg_size} depth={rec.depth_w}x{rec.depth_h} bytes={rec.depth_size}",
        f"intrinsics fx={rec.fx:.2f} fy={rec.fy:.2f} cx={rec.cx:.2f} cy={rec.cy:.2f}",
        f"pose q=({rec.qx:.4f}, {rec.qy:.4f}, {rec.qz:.4f}, {rec.qw:.4f})",
        f"pose t=({rec.tx:.4f}, {rec.ty:.4f}, {rec.tz:.4f})",
    ]
    y = 28
    for line in lines:
        cv2.putText(
            frame_bgr,
            line,
            (12, y),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.55,
            (0, 255, 0),
            2,
            cv2.LINE_AA,
        )
        y += 24
    return frame_bgr


def main() -> int:
    parser = argparse.ArgumentParser(description="Read VLPREC1 dataset files (JPEG body).")
    parser.add_argument("--input", required=True, help="Path to .rec file")
    parser.add_argument("--max-frames", type=int, default=0, help="Stop after N frames (0 = all)")
    parser.add_argument("--export-dir", default="", help="Optional output directory to export JPEG bodies")
    parser.add_argument("--csv", default="", help="Optional CSV output path for frame metadata")
    parser.add_argument("--display", action="store_true", help="Display frames in realtime with pose/intrinsics overlay")
    parser.add_argument("--speed", type=float, default=1.0, help="Playback speed multiplier for --display")
    parser.add_argument("--save-video", default="", help="Optional output video path; when set, render panels to video instead of cvshow")
    parser.add_argument("--video-fps", type=float, default=30.0, help="Output video FPS for --save-video")
    parser.add_argument("--log-every", type=int, default=60, help="Print progress every N frames (0 to disable periodic logs)")
    args = parser.parse_args()
    if args.speed <= 0:
        raise ValueError("--speed must be > 0")
    if args.video_fps <= 0:
        raise ValueError("--video-fps must be > 0")

    frame_count = 0
    bad_magic = 0
    bad_jpeg = 0
    first_ts = None
    last_ts = None
    writer = None
    csv_file = None

    if args.csv:
        os.makedirs(os.path.dirname(args.csv) or ".", exist_ok=True)
        csv_file = open(args.csv, "w", newline="")
        writer = csv.writer(csv_file)
        writer.writerow(
            [
                "index",
                "rel_ns",
                "timestamp_ns",
                "width",
                "height",
                "payload_size",
                "body_size",
                "jpeg_size",
                "depth_w",
                "depth_h",
                "depth_size",
                "magic_ok",
                "jpeg_ok",
                "fx",
                "fy",
                "cx",
                "cy",
                "qx",
                "qy",
                "qz",
                "qw",
                "tx",
                "ty",
                "tz",
                "export_path",
            ]
        )

    do_display = args.display or bool(args.save_video)
    playback_start_wall = None
    playback_start_rel_ns = None
    paused_accum_sec = 0.0
    pause_start = None
    writer_video = None
    cv2 = None
    ensure_size_nearest = None
    draw_header_text = None
    make_triptych_panel = None
    depth_msg_to_bgr_turbo = None
    draw_traj_canvas = None
    traj_x = []
    traj_z = []
    paused = False
    window_name = "VLPREC Viewer"
    do_show = args.display and not bool(args.save_video)
    if do_display:
        try:
            import cv2 as _cv2  # type: ignore

            cv2 = _cv2
            ros2_dir = os.path.join(os.path.dirname(__file__), "ros2")
            if ros2_dir not in sys.path:
                sys.path.append(ros2_dir)
            from utils import draw_header_text as _draw_header_text  # type: ignore
            from utils import draw_traj_canvas as _draw_traj_canvas  # type: ignore
            from utils import depth_msg_to_bgr_turbo as _depth_msg_to_bgr_turbo  # type: ignore
            from utils import ensure_size_nearest as _ensure_size_nearest  # type: ignore
            from utils import make_triptych_panel as _make_triptych_panel  # type: ignore

            draw_header_text = _draw_header_text
            draw_traj_canvas = _draw_traj_canvas
            depth_msg_to_bgr_turbo = _depth_msg_to_bgr_turbo
            ensure_size_nearest = _ensure_size_nearest
            make_triptych_panel = _make_triptych_panel
            if do_show:
                cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
        except Exception as e:
            print(f"Display disabled: failed to import/open display deps ({e})")
            do_display = False

    try:
        for rec in iter_vlprec(args.input):
            frame_count += 1
            if not rec.magic_ok:
                bad_magic += 1
            jpeg_ok = _looks_like_jpeg(rec.body)
            if not jpeg_ok:
                bad_jpeg += 1
            if first_ts is None:
                first_ts = rec.timestamp_ns
            last_ts = rec.timestamp_ns

            export_path = ""
            if args.export_dir:
                export_path = export_body(rec, args.export_dir)

            should_log = False
            if args.log_every > 0 and (frame_count == 1 or frame_count % args.log_every == 0):
                should_log = True
            if not rec.magic_ok or not jpeg_ok:
                should_log = True
            if should_log:
                print(
                    f"[{rec.index:06d}] rel={rec.rel_ns}ns ts={rec.timestamp_ns} "
                    f"{rec.width}x{rec.height} body={len(rec.body)} "
                    f"depth={rec.depth_w}x{rec.depth_h}/{rec.depth_size} "
                    f"magic={'ok' if rec.magic_ok else hex(rec.magic)}({rec.magic_kind}) "
                    f"jpeg={'yes' if jpeg_ok else 'no'}"
                )

            if writer is not None:
                writer.writerow(
                    [
                        rec.index,
                        rec.rel_ns,
                        rec.timestamp_ns,
                        rec.width,
                        rec.height,
                        rec.payload_size,
                        len(rec.body),
                        rec.jpeg_size,
                        rec.depth_w,
                        rec.depth_h,
                        rec.depth_size,
                        int(rec.magic_ok),
                        int(jpeg_ok),
                        rec.fx,
                        rec.fy,
                        rec.cx,
                        rec.cy,
                        rec.qx,
                        rec.qy,
                        rec.qz,
                        rec.qw,
                        rec.tx,
                        rec.ty,
                        rec.tz,
                        export_path,
                    ]
                )

            if do_display and cv2 is not None and ensure_size_nearest is not None and depth_msg_to_bgr_turbo is not None and draw_traj_canvas is not None and make_triptych_panel is not None and draw_header_text is not None and jpeg_ok:
                frame_bgr = _decode_jpeg(rec)
                if frame_bgr is not None:
                    frame_bgr = _draw_overlay(frame_bgr, rec)
                    if rec.depth_w > 0 and rec.depth_h > 0 and rec.depth_bytes:
                        depth_bgr = depth_msg_to_bgr_turbo(rec.depth_bytes, rec.depth_w, rec.depth_h, "16UC1")
                    else:
                        depth_bgr = None
                    h, w = frame_bgr.shape[:2]
                    if depth_bgr is None:
                        depth_bgr = cv2.cvtColor(
                            cv2.resize(
                                cv2.convertScaleAbs(cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2GRAY), alpha=0.0),
                                (w, h),
                                interpolation=cv2.INTER_NEAREST,
                            ),
                            cv2.COLOR_GRAY2BGR,
                        )
                    else:
                        depth_bgr = ensure_size_nearest(depth_bgr, w, h)
                    traj = draw_traj_canvas(traj_x, traj_z, w, h)
                    panel = make_triptych_panel(frame_bgr, depth_bgr, traj)
                    draw_header_text(panel, f"vlprec keys: space pause, q quit")
                    if writer_video is None and args.save_video:
                        os.makedirs(os.path.dirname(args.save_video) or ".", exist_ok=True)
                        fourcc = cv2.VideoWriter_fourcc(*"mp4v")
                        writer_video = cv2.VideoWriter(
                            args.save_video, fourcc, float(args.video_fps), (panel.shape[1], panel.shape[0])
                        )
                        if not writer_video.isOpened():
                            raise RuntimeError(f"Failed to open video writer: {args.save_video}")
                    if writer_video is not None:
                        writer_video.write(panel)
                    elif do_show:
                        cv2.imshow(window_name, panel)

            if do_display and cv2 is not None:
                traj_x.append(rec.tx)
                traj_z.append(rec.ty)
                if playback_start_wall is None:
                    playback_start_wall = time.time()
                    playback_start_rel_ns = rec.rel_ns
                target_elapsed_sec = (rec.rel_ns - playback_start_rel_ns) / 1e9 / args.speed
                target_wall = playback_start_wall + paused_accum_sec + target_elapsed_sec
                if do_show:
                    while time.time() < target_wall:
                        key = cv2.waitKey(1) & 0xFF
                        if key == ord("q"):
                            print("Quit requested by user.")
                            return 0
                        if key == ord(" "):
                            paused = not paused
                        if paused:
                            if pause_start is None:
                                pause_start = time.time()
                            key2 = cv2.waitKey(30) & 0xFF
                            if key2 == ord("q"):
                                print("Quit requested by user.")
                                return 0
                            if key2 == ord(" "):
                                paused = False
                                paused_accum_sec += time.time() - pause_start
                                pause_start = None
                        else:
                            if pause_start is not None:
                                paused_accum_sec += time.time() - pause_start
                                pause_start = None
                    key = cv2.waitKey(1) & 0xFF
                    if key == ord("q"):
                        print("Quit requested by user.")
                        return 0
                    if key == ord(" "):
                        paused = not paused

            if args.max_frames > 0 and frame_count >= args.max_frames:
                break
    finally:
        if csv_file is not None:
            csv_file.close()
        if writer_video is not None:
            writer_video.release()
            print(f"video_saved: {args.save_video}")
        if do_display and cv2 is not None:
            try:
                cv2.destroyAllWindows()
            except Exception:
                pass

    if frame_count == 0:
        print("No frames found.")
        return 1

    duration_ns = 0 if first_ts is None or last_ts is None else max(0, last_ts - first_ts)
    fps = (frame_count * 1e9 / duration_ns) if duration_ns > 0 else 0.0
    print("----")
    print(f"frames: {frame_count}")
    print(f"bad_frame_magic: {bad_magic}")
    print(f"bad_jpeg_body: {bad_jpeg}")
    print(f"duration_ns: {duration_ns}")
    print(f"avg_fps_by_ts: {fps:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
