# C++ gRPC Stream Client (Bazel)

This folder includes a C++ reimplementation of the Python stream client:

- `ws_stream_client.cc`: receives JPEG frames from `/vlp.FrameStreamService/StreamFrames`, decodes with OpenCV, and overlays:
  - timestamp
  - FPS
  - camera intrinsics (`fx fy cx cy`)
  - camera pose (`qx qy qz qw tx ty tz`)

## Build

```bash
bazel build //bazel:ws_stream_client
```

Binary output:

```bash
./bazel-bin/bazel/ws_stream_client
```

## Run with Android phone

Forward gRPC port:

```bash
adb forward tcp:50051 tcp:50051
```

Run:

```bash
bazel build //mapping:ws_stream_client

./bazel-bin/mapping/ws_stream_client \
--da3_model=http://192.168.19.119:8000/v2/models/depthanything3_trt/infer \
--host=192.168.19.153 --port=50051 --logtostderr=1 --enable_websocket=true --websocket_port=9002
```

Options:

- `--window-width` default `640`
- `--window-height` default `480`

Press `q` in the OpenCV window to quit.
