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
./bazel-bin/bazel/ws_stream_client \
  --host=127.0.0.1 --port=50051 \
  --window_width=640 --window_height=480 \
  --da3_model=python/models/da3_small_2_392x224_sim.onnx \
  --keyframe_rot_deg=6.0 --keyframe_trans_m=0.12 \
  --logtostderr=1
```

Options:

- `--window-width` default `640`
- `--window-height` default `480`

Press `q` in the OpenCV window to quit.
