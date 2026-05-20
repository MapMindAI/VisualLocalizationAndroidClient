#!/usr/bin/env python3
import argparse
import struct
import time
from typing import Iterator, Tuple

import cv2
import grpc
import numpy as np
from concurrent import futures


FILE_MAGIC = b"VLPREC1\n"
FILE_VERSION = 1
FILE_HEADER_STRUCT = struct.Struct("<8sI")
ENTRY_HEADER_STRUCT = struct.Struct("<QI")

FRAME_MAGIC = 0x564C5032  # "VLP2"
FRAME_HEADER_STRUCT = struct.Struct("<IQII11f")
FRAME_HEADER_SIZE = FRAME_HEADER_STRUCT.size


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


def main() -> int:
  parser = argparse.ArgumentParser(
      description="Replay recorded gRPC stream at original receive-time intervals."
  )
  parser.add_argument("--input", required=True, help="Recorded file path")
  parser.add_argument("--speed", type=float, default=1.0, help="Playback speed (e.g. 2.0)")
  parser.add_argument("--loop", action="store_true", help="Loop playback")
  parser.add_argument("--no-display", action="store_true", help="Disable OpenCV display")
  parser.add_argument(
      "--serve-port",
      type=int,
      default=0,
      help="If >0, run gRPC server on this port and stream replay to clients.",
  )
  args = parser.parse_args()

  if args.speed <= 0:
    raise ValueError("--speed must be > 0")

  if args.serve_port > 0:
    return _serve_grpc(args.input, args.speed, args.loop, args.serve_port)

  window_name = "VLP Playback"
  if not args.no_display:
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window_name, 960, 540)

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
        time.sleep(min(0.005, remain_ns / 1e9))

      if len(payload) < FRAME_HEADER_SIZE:
        continue
      unpacked = FRAME_HEADER_STRUCT.unpack_from(payload, 0)
      magic = unpacked[0]
      if magic != FRAME_MAGIC:
        continue
      timestamp_ns = unpacked[1]
      jpeg_bytes = payload[FRAME_HEADER_SIZE:]
      if jpeg_bytes and not args.no_display:
        img = cv2.imdecode(np.frombuffer(jpeg_bytes, dtype=np.uint8), cv2.IMREAD_COLOR)
        if img is not None:
          cv2.putText(
              img,
              f"play_ts(ns): {timestamp_ns}  speed: {args.speed:.2f}x",
              (10, 28),
              cv2.FONT_HERSHEY_SIMPLEX,
              0.7,
              (0, 255, 0),
              2,
              cv2.LINE_AA,
          )
          cv2.imshow(window_name, img)
          if (cv2.waitKey(1) & 0xFF) == ord("q"):
            if not args.no_display:
              cv2.destroyAllWindows()
            return 0

      played += 1
      if played % 30 == 0:
        print(f"[{played}] timestamp_ns={timestamp_ns}")

    print(f"Playback finished. Frames: {played}")
    if not args.loop:
      break
    print("Looping...")

  if not args.no_display:
    cv2.destroyAllWindows()
  return 0


def _stream_payloads(path: str, speed: float, loop: bool) -> Iterator[bytes]:
  while True:
    playback_start_ns = time.monotonic_ns()
    first_rel_ns = None
    played = 0
    for rel_ns, payload in _iter_entries(path):
      if first_rel_ns is None:
        first_rel_ns = rel_ns
      target_rel_ns = int((rel_ns - first_rel_ns) / speed)
      while True:
        elapsed_ns = time.monotonic_ns() - playback_start_ns
        remain_ns = target_rel_ns - elapsed_ns
        if remain_ns <= 0:
          break
        time.sleep(min(0.005, remain_ns / 1e9))
      played += 1
      if played % 30 == 0:
        print(f"[serve {played}] streamed")
      yield payload
    if not loop:
      break
    print("Replay loop restart.")


def _serve_grpc(path: str, speed: float, loop: bool, port: int) -> int:
  method = grpc.unary_stream_rpc_method_handler(
      lambda request, context: _stream_payloads(path, speed, loop),
      request_deserializer=lambda x: x,
      response_serializer=lambda x: x,
  )
  handler = grpc.method_handlers_generic_handler(
      "vlp.FrameStreamService",
      {"StreamFrames": method},
  )
  server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
  server.add_generic_rpc_handlers((handler,))
  server.add_insecure_port(f"[::]:{port}")
  server.start()
  print(f"gRPC replay server listening on 0.0.0.0:{port}")
  print("Endpoint: /vlp.FrameStreamService/StreamFrames")
  print(f"Replay source: {path} speed={speed:.2f} loop={loop}")
  try:
    while True:
      time.sleep(1.0)
  except KeyboardInterrupt:
    print("Stopping server.")
  finally:
    server.stop(grace=0)
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
