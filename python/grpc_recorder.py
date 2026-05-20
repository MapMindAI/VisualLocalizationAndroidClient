#!/usr/bin/env python3
import argparse
import struct
import time

import grpc


FILE_MAGIC = b"VLPREC1\n"
FILE_VERSION = 1
FILE_HEADER_STRUCT = struct.Struct("<8sI")
ENTRY_HEADER_STRUCT = struct.Struct("<QI")


def main() -> int:
  parser = argparse.ArgumentParser(
      description="Record gRPC frame stream with receive-time intervals."
  )
  parser.add_argument("--host", default="127.0.0.1", help="gRPC host")
  parser.add_argument("--port", type=int, default=50051, help="gRPC port")
  parser.add_argument("--output", required=True, help="Output recording file path")
  parser.add_argument("--subscribe", default="subscribe", help="Subscribe request payload")
  args = parser.parse_args()

  target = f"{args.host}:{args.port}"
  channel = grpc.insecure_channel(target)
  stream_frames = channel.unary_stream(
      "/vlp.FrameStreamService/StreamFrames",
      request_serializer=lambda x: x,
      response_deserializer=lambda x: x,
  )

  print(f"Connecting to {target}")
  frame_count = 0
  start_ns = 0
  try:
    with open(args.output, "wb") as f:
      f.write(FILE_HEADER_STRUCT.pack(FILE_MAGIC, FILE_VERSION))
      print(f"Recording to {args.output}")
      for payload in stream_frames(args.subscribe.encode("utf-8")):
        now_ns = time.monotonic_ns()
        if start_ns == 0:
          start_ns = now_ns
        rel_ns = now_ns - start_ns
        f.write(ENTRY_HEADER_STRUCT.pack(rel_ns, len(payload)))
        f.write(payload)
        frame_count += 1
        if frame_count % 30 == 0:
          elapsed_s = rel_ns / 1e9
          fps = frame_count / elapsed_s if elapsed_s > 0 else 0.0
          print(f"[{frame_count}] rec_fps={fps:.2f}")
  except KeyboardInterrupt:
    print("Stopped by user.")
  except grpc.RpcError as e:
    print(f"gRPC stream error: {e.code().name} {e.details()}")
    return 1
  finally:
    channel.close()

  print(f"Done. Saved {frame_count} frames.")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
