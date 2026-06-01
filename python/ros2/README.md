
## Looper

[LooperRobotics Insight 9](https://looper-robotics.com/),
[LooperRobotics Youtube](https://www.youtube.com/channel/UCJjQn8wxSn_McxOjN8EYM4g)

## Looper rosbag

example bag : data/extreme_movement/rosbag

full topics

```
/camera/camera/color/camera_info
/camera/camera/color/image_raw/compressed
/camera/camera/color/image_rect_raw/compressed
/camera/camera/depth/image_rect_raw
/camera/camera/imu
/camera/camera/infra1/camera_info
/camera/camera/infra1/image_raw
/camera/camera/infra1/image_rect_raw
/camera/camera/infra2/camera_info
/camera/camera/infra2/image_raw
/camera/camera/infra2/image_rect_raw
/camera/camera/vio_100hz
/camera/camera/vio_20hz
/camera/camera/vio_status
```

```
ros2 bag record -o test_bag \
/camera/camera/color/image_rect_raw/compressed /camera/camera/depth/image_rect_raw /camera/camera/vio_100hz \
/camera/camera/color/camera_info /camera/camera/infra1/camera_info /tf_static
```

## Compare with ARCore data

example rec: data/extreme_movement/2026-05-29_11-19-06/vlp_stream.rec

## Visualize ROS bag like vlprec_reader

Display 3 panes in OpenCV: RGB, depth, and `vio_20hz` trajectory.

```bash
python3 python/ros2/ros2_bag_viewer.py \
  --bag data/extreme_movement/rosbag
```

Useful options:

- `--speed 1.0` real-time replay
- `--max-frames 300` stop early
- `--save-video out/ros2_bag_view.mp4` save triptych video instead of live display
- `--video-fps 30` set output video FPS when saving
- `--log-every 60` print progress every N rendered frames

Save video example:

```bash
python3 python/ros2/ros2_bag_viewer.py \
  --bag data/${SESSION}/rosbag \
  --save-video data/${SESSION}/ros2_bag_triptych.mp4 \
  --video-fps 30
```

## Live ROS2 subscribe and display

Subscribe in real time and show 3 panes: RGB, depth, `vio_20hz` trajectory.

```bash
python3 python/ros2/ros2_live_viewer.py
```

Optional topic overrides:

```bash
python3 python/ros2/ros2_live_viewer.py \
  --color-topic /camera/camera/color/image_rect_raw/compressed \
  --depth-topic /camera/camera/depth/image_rect_raw \
  --vio-topic /camera/camera/vio_20hz
```

## Convert ROS2 bag to VLPREC data format

Convert rosbag color/depth/pose into `.rec` (VLPREC1 + VLP2 payload + DPT1 trailer).
No image/depth rotation is applied.

```bash
python3 python/ros2/ros2_bag_to_vlpdata.py \
  --bag data/${SESSION}/rosbag \
  --output data/${SESSION}/rosbag_to_vlp.rec
```

Notes:
- Updated trailer format is `DPT2` (backward-compatible reader supports `DPT1` and `DPT2`).
- VLP2 header intrinsics store RGB intrinsics.
- `DPT2` trailer stores depth intrinsics (`depth_fx/depth_fy/depth_cx/depth_cy`).
- Depth intrinsics source:
  1. `/camera/camera/depth/camera_info` when present
  2. derived by resizing RGB intrinsics when depth intrinsics are missing
  3. fallback defaults:
  - `fx=313.94085693359375`
  - `fy=313.94085693359375`
  - `cx=269.742431640625`
  - `cy=316.34063720703125`
