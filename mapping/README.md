# Mapping Pipeline (gRPC + DA3 + Voxblox + Web Viewer)

This directory contains the C++ online mapping client that:

1. Receives JPEG frames and camera pose from `/vlp.FrameStreamService/StreamFrames`.
2. Runs DA3 depth on keyframe pairs.
3. Integrates latest RGBD + pose into Voxblox.
4. Streams image/trajectory/ESDF to the built-in web viewer.

Main entry:

- `mono_vio_ws_stream_client.cc`
- `dataset_rgbd_player.cc` (offline replay from timestamped RGB/depth/pose files)

## Run with Android phone

Demo:

https://github.com/user-attachments/assets/9f29c5c9-f34a-402a-b798-6b70874babde

Forward gRPC port:

```bash
adb forward tcp:50051 tcp:50051
```

Run:

```bash
./bazel-bin/mapping/mono_vio_ws_stream_client \
  --host=127.0.0.1 \
  --port=50051 \
  --logtostderr=1 \
  --enable_websocket=true \
  --websocket_port=9002 \
  --enable_voxblox=true
```

Open viewer:

```text
http://127.0.0.1:9002/index.html
```

## Runtime behavior

- Keyframe gating: controlled by rotation/translation thresholds.
- DA3: produces depth from keyframe pairs and scale refinement.
- Voxblox: integrates depth + pose, updates ESDF.
- WebSocket payload:
  - always: overlay image, current pose, trajectory, DA3/Voxblox status
  - ESDF points: sent only when ESDF is updated
- Web viewer:
  - updates ESDF render only when `esdf_points` is received
  - color mode options: `By distance`, `Constant`, `By Height`
  - `follow camera` moves with trajectory while keeping manual rotate enabled

## Common flags

Connection:

- `--host` gRPC server host
- `--port` gRPC server port

DA3:

- `--da3_model` ONNX path
- `--da3_width` model input width
- `--da3_height` model input height
- `--da3_scale_refine_depth_max_m` max depth used in scale refinement

Voxblox:

- `--enable_voxblox`
- `--voxblox_voxel_size_m`

Keyframe:

- `--keyframe_rot_deg`
- `--keyframe_trans_m`

Web:

- `--enable_websocket`
- `--websocket_port`
- `--web_send_hz`
- `--web_max_traj_points`
- `--web_client_html`

## Offline dataset replay

Binary:

```text
//mapping:dataset_rgbd_player
```

Input CSV format:

- `rgb.csv`: `timestamp_ns,path_to_rgb_image`
- `depth.csv`: `timestamp_ns,path_to_depth_image`
- `pose.csv`: `timestamp_ns,qx,qy,qz,qw,tx,ty,tz`

Notes:

- Timestamps do not need to be aligned across RGB/depth/pose files.
- Replay merges all events by timestamp and always displays latest RGB and latest depth.
- Depth image can be `CV_16U` (typical mm) or `CV_32F` (meters).

Run:

```bash
./bazel-bin/mapping/dataset_rgbd_player \
  --dataset_dir=/path/to/dataset \
  --speed=1.0 \
  --loop=true \
  --enable_websocket=true \
  --websocket_port=9002 \
  --depth_scale=0.001 \
  --depth_vis_max_m=5.0
```

Expected dataset layout:

```text
/path/to/dataset/
  rgb.csv
  depth.csv
  pose.csv
```

Useful flags:

- `--dataset_dir`: folder containing `rgb.csv`, `depth.csv`, `pose.csv`
- `--speed`: replay speed multiplier
- `--loop`: replay continuously
- `--depth_scale`: scale factor for integer depth images (e.g. `0.001` for mm -> m)
- `--window_view`: OpenCV window name for side-by-side RGBD view
- `--render_max_size`: max width/height of rendered combined image (default `1080`)
- `--enable_opencv_viz`: enable local OpenCV window (default `false`)
- `--enable_websocket`: stream output to `web_client.html` schema (default `true`)
- `--websocket_port`, `--web_client_html`: web viewer serving options

Optional overrides:

- `--rgb_csv`, `--depth_csv`, `--pose_csv` can override individual files.
- Relative image paths inside CSV are resolved relative to each CSV file location.
