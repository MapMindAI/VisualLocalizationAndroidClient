#!/usr/bin/env python3
import argparse
import csv
import os
import struct
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


def _depth_preview(record: FrameRecord):
    try:
        import cv2  # type: ignore
        import numpy as np  # type: ignore

        if record.depth_w <= 0 or record.depth_h <= 0:
            return None
        expected = record.depth_w * record.depth_h * 2
        if len(record.depth_bytes) < expected:
            return None
        d16 = np.frombuffer(record.depth_bytes[:expected], dtype=np.uint16).reshape(
            (record.depth_h, record.depth_w)
        )
        p99 = float(np.percentile(d16, 99))
        depth_u8 = cv2.convertScaleAbs(d16, alpha=255.0 / max(1.0, p99))
        return cv2.applyColorMap(depth_u8, cv2.COLORMAP_TURBO)
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


def _update_traj_plot(ax, xs, ys, zs):
    ax.cla()
    ax.plot(xs, ys, zs, color="tab:blue", linewidth=2.0)
    ax.scatter([xs[-1]], [ys[-1]], [zs[-1]], color="tab:red", s=24)
    ax.set_xlabel("x")
    ax.set_ylabel("y")
    ax.set_zlabel("z")
    ax.set_title("Camera Trajectory")
    x_min, x_max = min(xs), max(xs)
    y_min, y_max = min(ys), max(ys)
    z_min, z_max = min(zs), max(zs)
    span = max(x_max - x_min, y_max - y_min, z_max - z_min, 1e-3)
    pad = 0.1 * span
    ax.set_xlim(x_min - pad, x_max + pad)
    ax.set_ylim(y_min - pad, y_max + pad)
    ax.set_zlim(z_min - pad, z_max + pad)


def main() -> int:
    parser = argparse.ArgumentParser(description="Read VLPREC1 dataset files (JPEG body).")
    parser.add_argument("--input", required=True, help="Path to .rec file")
    parser.add_argument("--max-frames", type=int, default=0, help="Stop after N frames (0 = all)")
    parser.add_argument("--export-dir", default="", help="Optional output directory to export JPEG bodies")
    parser.add_argument("--csv", default="", help="Optional CSV output path for frame metadata")
    parser.add_argument("--display", action="store_true", help="Display frames in realtime with pose/intrinsics overlay")
    parser.add_argument("--speed", type=float, default=1.0, help="Playback speed multiplier for --display")
    args = parser.parse_args()
    if args.speed <= 0:
        raise ValueError("--speed must be > 0")

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

    do_display = args.display
    paused = False
    prev_rel_ns = None
    cv2 = None
    plt_mod = None
    fig = None
    image_ax = None
    image_artist = None
    depth_ax = None
    depth_artist = None
    traj_ax = None
    traj_x = []
    traj_y = []
    traj_z = []
    stop_requested = False
    if do_display:
        try:
            import cv2 as _cv2  # type: ignore

            cv2 = _cv2
            import matplotlib.pyplot as _plt  # type: ignore

            plt_mod = _plt
            fig = plt_mod.figure("VLPREC Viewer + Trajectory", figsize=(16, 6))
            gs = fig.add_gridspec(1, 3, width_ratios=[1, 1, 1])
            image_ax = fig.add_subplot(gs[0, 0])
            depth_ax = fig.add_subplot(gs[0, 1])
            traj_ax = fig.add_subplot(gs[0, 2], projection="3d")
            traj_ax.view_init(elev=-75-180, azim=-20, roll=70)

            image_ax.set_title("RGB")
            image_ax.axis("off")
            depth_ax.set_title("Depth")
            depth_ax.axis("off")
            plt_mod.ion()
            control = {"paused": False, "quit": False}

            def _on_key(event):
                if event.key == "q":
                    control["quit"] = True
                elif event.key == " ":
                    control["paused"] = not control["paused"]

            fig.canvas.mpl_connect("key_press_event", _on_key)
            fig._vlp_control = control  # type: ignore[attr-defined]
            plt_mod.show(block=False)
        except Exception as e:
            print(f"Display disabled: failed to import/open display deps ({e})")
            do_display = False

    try:
        for rec in iter_vlprec(args.input):
            if stop_requested:
                print("Quit requested by user.")
                return 0
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

            if do_display and cv2 is not None and plt_mod is not None and image_ax is not None and traj_ax is not None and jpeg_ok:
                frame_bgr = _decode_jpeg(rec)
                if frame_bgr is not None:
                    frame_bgr = _draw_overlay(frame_bgr, rec)
                    depth_bgr = _depth_preview(rec)
                    frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
                    if image_artist is None:
                        image_artist = image_ax.imshow(frame_rgb)
                    else:
                        image_artist.set_data(frame_rgb)
                    image_ax.set_title("RGB (press space: pause, q: quit)")
                    if depth_ax is not None:
                        h, w = frame_bgr.shape[:2]
                        if depth_bgr is not None:
                            depth_bgr = cv2.resize(depth_bgr, (w, h), interpolation=cv2.INTER_NEAREST)
                        else:
                            depth_bgr = cv2.cvtColor(
                                cv2.resize(
                                    cv2.convertScaleAbs(
                                        cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2GRAY), alpha=0.0
                                    ),
                                    (w, h),
                                    interpolation=cv2.INTER_NEAREST,
                                ),
                                cv2.COLOR_GRAY2BGR,
                            )
                        depth_rgb = cv2.cvtColor(depth_bgr, cv2.COLOR_BGR2RGB)
                        if depth_artist is None:
                            depth_artist = depth_ax.imshow(depth_rgb)
                        else:
                            depth_artist.set_data(depth_rgb)

                    if prev_rel_ns is not None:
                        dt_sec = max(0.0, (rec.rel_ns - prev_rel_ns) / 1e9 / args.speed)
                        if dt_sec > 0:
                            end_t = time.time() + dt_sec
                            while time.time() < end_t:
                                ctl = getattr(fig, "_vlp_control", {"paused": False, "quit": False}) if fig is not None else {"paused": False, "quit": False}
                                if ctl["quit"]:
                                    stop_requested = True
                                    break
                                while ctl["paused"]:
                                    plt_mod.pause(0.03)
                                    ctl = getattr(fig, "_vlp_control", ctl) if fig is not None else ctl
                                    if ctl["quit"]:
                                        stop_requested = True
                                        break
                                plt_mod.pause(0.001)
                            if stop_requested:
                                print("Quit requested by user.")
                                return 0
                prev_rel_ns = rec.rel_ns
            if do_display and plt_mod is not None and traj_ax is not None:
                traj_x.append(rec.tx)
                traj_y.append(rec.ty)
                traj_z.append(-rec.tz)
                _update_traj_plot(traj_ax, traj_x, traj_y, traj_z)
                plt_mod.pause(0.001)

            if args.max_frames > 0 and frame_count >= args.max_frames:
                break
    finally:
        if csv_file is not None:
            csv_file.close()
        if do_display and plt_mod is not None:
            try:
                plt_mod.ioff()
                plt_mod.close("all")
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
