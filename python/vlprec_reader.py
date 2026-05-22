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
FRAME_MAGIC = 0x564C5032
FRAME_MAGIC_ASCII = b"VLP2"
FRAME_HEADER_SIZE = 64


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
    body: bytes


def _read_exact(f, n: int) -> Optional[bytes]:
    data = f.read(n)
    if not data:
        return None
    if len(data) != n:
        raise ValueError(f"Unexpected EOF: expected {n} bytes, got {len(data)}")
    return data


def _parse_frame_header(payload: bytes):
    if len(payload) < FRAME_HEADER_SIZE:
        raise ValueError("Payload too small for frame header")

    magic_raw = struct.unpack_from("<I", payload, 0)[0]

    # Current format: ASCII "VLP2" at [0:4], timestamp at offset 8.
    if payload[0:4] == FRAME_MAGIC_ASCII:
        timestamp_ns = struct.unpack_from("<Q", payload, 8)[0]
        width = struct.unpack_from("<I", payload, 16)[0]
        height = struct.unpack_from("<I", payload, 20)[0]
        fx, fy, cx, cy = struct.unpack_from("<4f", payload, 24)
        qx, qy, qz, qw, tx, ty, tz = struct.unpack_from("<7f", payload, 40)
        return True, "ascii", magic_raw, (
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
        )

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
    return (magic == FRAME_MAGIC), "legacy", magic, (
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
    )


def _looks_like_jpeg(data: bytes) -> bool:
    return len(data) >= 4 and data[0:2] == b"\xFF\xD8" and data[-2:] == b"\xFF\xD9"


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
            if payload_size < FRAME_HEADER_SIZE:
                raise ValueError(
                    f"Payload too small at frame {idx}: {payload_size} < {FRAME_HEADER_SIZE}"
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
            ) = fields
            body = payload[FRAME_HEADER_SIZE:]
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
        f"intrinsics fx={rec.fx:.2f} fy={rec.fy:.2f} cx={rec.cx:.2f} cy={rec.cy:.2f}",
        f"pose q=({rec.qx:.4f}, {rec.qy:.4f}, {rec.qz:.4f}, {rec.qw:.4f})",
        f"pose t=({rec.tx:.4f}, {rec.ty:.4f}, {rec.tz:.4f})",
        "keys: q=quit, space=pause/play",
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
    window_name = "VLPREC Viewer"
    cv2 = None
    if do_display:
        try:
            import cv2 as _cv2  # type: ignore

            cv2 = _cv2
            cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
        except Exception as e:
            print(f"Display disabled: failed to import/open OpenCV ({e})")
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

            print(
                f"[{rec.index:06d}] rel={rec.rel_ns}ns ts={rec.timestamp_ns} "
                f"{rec.width}x{rec.height} body={len(rec.body)} "
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

            if do_display and cv2 is not None and jpeg_ok:
                frame_bgr = _decode_jpeg(rec)
                if frame_bgr is not None:
                    frame_bgr = _draw_overlay(frame_bgr, rec)
                    cv2.imshow(window_name, frame_bgr)

                    if prev_rel_ns is not None:
                        dt_sec = max(0.0, (rec.rel_ns - prev_rel_ns) / 1e9 / args.speed)
                        if dt_sec > 0:
                            end_t = time.time() + dt_sec
                            while time.time() < end_t:
                                key = cv2.waitKey(1) & 0xFF
                                if key == ord("q"):
                                    print("Quit requested by user.")
                                    return 0
                                if key == ord(" "):
                                    paused = not paused
                                while paused:
                                    key2 = cv2.waitKey(30) & 0xFF
                                    if key2 == ord("q"):
                                        print("Quit requested by user.")
                                        return 0
                                    if key2 == ord(" "):
                                        paused = False
                                        break
                    key = cv2.waitKey(1) & 0xFF
                    if key == ord("q"):
                        print("Quit requested by user.")
                        return 0
                    if key == ord(" "):
                        paused = not paused
                prev_rel_ns = rec.rel_ns

            if args.max_frames > 0 and frame_count >= args.max_frames:
                break
    finally:
        if csv_file is not None:
            csv_file.close()
        if do_display and cv2 is not None:
            cv2.destroyAllWindows()

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
