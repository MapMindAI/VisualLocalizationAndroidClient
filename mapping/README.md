# Mapping Pipeline (Replay + Voxblox + ROS2 + Pangolin)

This directory contains the C++ online mapping client that:

1. Receives JPEG frames, depth and camera pose from `/vlp.FrameStreamService/StreamFrames`.
2. Integrates latest RGBD + pose into Voxblox.
3. Publishes debug image, RGBD cloud, ESDF cloud, and pose to ROS2 topics.

Main entry:

- `mono_vio_ws_stream_client.cc`
- `rosnodes/voxblox_ros_node.cc`
- `pangolin/pangolin_visualizer_node.cc`

## Run with Android phone

Demo:

https://github.com/user-attachments/assets/77e57cfb-af64-442e-8c0e-f65b7eb5f4ae

Run:

```bash
bazel build //mapping:mono_vio_ws_stream_client

./bazel-bin/mapping/mono_vio_ws_stream_client \
  --data_session=data/outdoor_large_circle/rosbag_to_vlp.rec \
  --logtostderr=1 \
  --max_depth_m=12.0 \
  --enable_websocket=true \
  --websocket_port=9002 \
  --enable_voxblox=true
```

Open viewer:

```text
http://127.0.0.1:9002/index.html
```

## Run with ROS bags

Run:

```bash
bazel build //mapping/rosnodes:voxblox_ros_node //mapping/pangolin:pangolin_visualizer_node

./bazel-bin/mapping/rosnodes/voxblox_ros_node \
  --topic_depth=/vlp/depth \
  --topic_pose=/vlp/pose \
  --topic_esdf=/vlp/esdf_cloud

./bazel-bin/mapping/pangolin/pangolin_visualizer_node \
  --topic_rgb=/vlp/rgb \
  --topic_depth=/vlp/depth \
  --topic_pose=/vlp/pose \
  --topic_esdf=/vlp/esdf_cloud
```

Notes:
- The recommended launcher is `mapping/run_rosnodes.sh` (starts both nodes and optional rosbag play).
