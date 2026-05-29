
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
ros2 bag record -o test_bag /camera/camera/color/image_rect_raw/compressed /camera/camera/depth/image_rect_raw /camera/camera/vio_100hz
```

## Compare with ARCore data

example rec: data/extreme_movement/2026-05-29_11-19-06/vlp_stream.rec

## Visualize ROS bag like vlprec_reader

Display 3 panes in matplotlib: RGB, depth, and `vio_20hz` trajectory.

```bash
python3 python/looper_compare/rosbag_viewer.py \
  --bag data/extreme_movement/rosbag
```

Useful options:

- `--speed 1.0` real-time replay
- `--max-frames 300` stop early

## Live ROS2 subscribe and display

Subscribe in real time and show 3 panes: RGB, depth, `vio_20hz` trajectory.

```bash
python3 python/looper_compare/ros2_live_viewer.py
```

Optional topic overrides:

```bash
python3 python/looper_compare/ros2_live_viewer.py \
  --color-topic /camera/camera/color/image_rect_raw/compressed \
  --depth-topic /camera/camera/depth/image_rect_raw \
  --vio-topic /camera/camera/vio_20hz
```
