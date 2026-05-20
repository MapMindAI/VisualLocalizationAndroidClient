# C++ gRPC Stream Client (Bazel)

This folder includes a C++ reimplementation of the Python stream client:

- `ws_stream_client.cc`: receives JPEG frames from `/vlp.FrameStreamService/StreamFrames`, decodes with OpenCV, and overlays:
  - timestamp
  - FPS
  - camera intrinsics (`fx fy cx cy`)
  - camera pose (`qx qy qz qw tx ty tz`)

## Run with Android phone

https://github.com/user-attachments/assets/9f29c5c9-f34a-402a-b798-6b70874babde


Forward gRPC port:

```bash
adb forward tcp:50051 tcp:50051
```

Build & Run:

```bash
bazel build //mapping:ws_stream_client

./bazel-bin/mapping/ws_stream_client \
--host=127.0.0.1 --port=50051 --logtostderr=1 \
--enable_websocket=true --websocket_port=9002 \
--enable_voxblox=true
```
