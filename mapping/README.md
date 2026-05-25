# Mapping Pipeline (gRPC + DA3 + Voxblox + Web Viewer)

This directory contains the C++ online mapping client that:

1. Receives JPEG frames, depth and camera pose from `/vlp.FrameStreamService/StreamFrames`.
2. Integrates latest RGBD + pose into Voxblox.
3. Streams image/trajectory/ESDF to the built-in web viewer.

Main entry:

- `mono_vio_ws_stream_client.cc`

## Run with Android phone

Demo:

https://github.com/user-attachments/assets/9f29c5c9-f34a-402a-b798-6b70874babde

Run:

```bash
bazel build //mapping:mono_vio_ws_stream_client

./bazel-bin/mapping/mono_vio_ws_stream_client \
  --data_session=data/files/2026-05-25_11-55-32/vlp_stream.rec \
  --logtostderr=1 \
  --enable_websocket=true \
  --websocket_port=9002 \
  --enable_voxblox=true
```

Open viewer:

```text
http://127.0.0.1:9002/index.html
```
