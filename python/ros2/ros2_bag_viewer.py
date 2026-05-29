#!/usr/bin/env python3
import argparse
import time
from dataclasses import dataclass
from typing import Optional

import numpy as np  # type: ignore
import rosbag2_py  # type: ignore
from rclpy.serialization import deserialize_message  # type: ignore
from rosidl_runtime_py.utilities import get_message  # type: ignore
from utils import decode_compressed_image_rgb, depth_msg_to_bgr_turbo, draw_traj_canvas, resize_nearest, rot90_ccw


@dataclass
class PoseState:
    stamp_ns: int
    x: float
    y: float
    z: float


def _stamp_to_ns(stamp) -> int:
    return int(stamp.sec) * 1_000_000_000 + int(stamp.nanosec)


def main() -> int:
    import cv2  # type: ignore
    ap = argparse.ArgumentParser(description="ROS2 bag viewer: color + depth + vio_20hz trajectory")
    ap.add_argument("--bag", required=True, help="Bag directory path (contains metadata.yaml)")
    ap.add_argument("--color-topic", default="/camera/camera/color/image_rect_raw/compressed")
    ap.add_argument("--depth-topic", default="/camera/camera/depth/image_rect_raw")
    ap.add_argument("--vio-topic", default="/camera/camera/vio_20hz")
    ap.add_argument("--speed", type=float, default=1.0, help="Playback speed multiplier")
    ap.add_argument("--max-frames", type=int, default=0)
    args = ap.parse_args()
    if args.speed <= 0:
        raise ValueError("--speed must be > 0")

    storage_options = rosbag2_py.StorageOptions(uri=args.bag, storage_id="sqlite3")
    converter_options = rosbag2_py.ConverterOptions(
        input_serialization_format="cdr",
        output_serialization_format="cdr",
    )
    reader = rosbag2_py.SequentialReader()
    reader.open(storage_options, converter_options)

    type_map = {t.name: t.type for t in reader.get_all_topics_and_types()}
    if args.color_topic not in type_map:
        raise ValueError(f"Missing color topic in bag: {args.color_topic}")
    if args.depth_topic not in type_map:
        print(f"Depth topic not found: {args.depth_topic}; depth panel will stay blank.")
    if args.vio_topic not in type_map:
        print(f"VIO topic not found: {args.vio_topic}; trajectory will stay empty.")

    msg_types = {name: get_message(typename) for name, typename in type_map.items()}

    latest_depth_rgb = np.zeros((480, 640, 3), dtype=np.uint8)
    latest_pose = None
    traj_x = []
    traj_y = []
    traj_z = []

    frame_count = 0
    start_msg_time_ns = None
    start_wall = None
    paused_accum_sec = 0.0
    pause_start = None
    interested_topics = {args.color_topic, args.depth_topic, args.vio_topic}

    paused = False
    window_name = "Looper ROS bag Viewer"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)

    while reader.has_next():
        topic, data, t_ns = reader.read_next()

        if topic not in interested_topics:
            continue

        if start_msg_time_ns is None:
            start_msg_time_ns = int(t_ns)
            start_wall = time.time()

        msg = deserialize_message(data, msg_types[topic])

        if topic == args.depth_topic:
            dimg = depth_msg_to_bgr_turbo(bytes(msg.data), int(msg.width), int(msg.height), str(msg.encoding))
            if dimg is not None:
                latest_depth_rgb = dimg
            continue

        if topic == args.vio_topic:
            p = msg.pose.position
            latest_pose = PoseState(stamp_ns=_stamp_to_ns(msg.header.stamp), x=float(p.x), y=float(p.y), z=float(p.z))
            traj_x.append(latest_pose.x)
            traj_y.append(latest_pose.y)
            traj_z.append(-latest_pose.z)
            continue

        if topic != args.color_topic:
            continue

        rgb = decode_compressed_image_rgb(bytes(msg.data))
        if rgb is None:
            continue
        rgb = rot90_ccw(rgb)

        rel_ns = int(t_ns) - start_msg_time_ns
        frame_count += 1
        depth_show = rot90_ccw(latest_depth_rgb)
        if depth_show.shape[0] != rgb.shape[0] or depth_show.shape[1] != rgb.shape[1]:
            depth_show = resize_nearest(depth_show, rgb.shape[1], rgb.shape[0])

        traj = draw_traj_canvas(traj_x, traj_z, rgb.shape[1], rgb.shape[0])
        rgb_bgr = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
        panel = np.hstack([rgb_bgr, depth_show, traj])
        if latest_pose is not None:
            txt = f"idx={frame_count-1} rel={rel_ns}ns vio=({latest_pose.x:.3f},{latest_pose.y:.3f},{latest_pose.z:.3f}) keys: space pause, q quit"
        else:
            txt = f"idx={frame_count-1} rel={rel_ns}ns keys: space pause, q quit"
        cv2.putText(panel, txt, (12, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2, cv2.LINE_AA)
        cv2.imshow(window_name, panel)

        target_elapsed = rel_ns / 1e9 / args.speed
        target_wall = start_wall + paused_accum_sec + target_elapsed
        while time.time() < target_wall:
            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                print("Quit requested by user.")
                cv2.destroyAllWindows()
                return 0
            if key == ord(" "):
                paused = not paused
            if paused:
                if pause_start is None:
                    pause_start = time.time()
                key2 = cv2.waitKey(30) & 0xFF
                if key2 == ord("q"):
                    print("Quit requested by user.")
                    cv2.destroyAllWindows()
                    return 0
                if key2 == ord(" "):
                    paused = False
                    paused_accum_sec += time.time() - pause_start
                    pause_start = None
            else:
                if pause_start is not None:
                    paused_accum_sec += time.time() - pause_start
                    pause_start = None

        key = cv2.waitKey(1) & 0xFF
        if key == ord("q"):
            print("Quit requested by user.")
            break
        if key == ord(" "):
            paused = not paused

        if args.max_frames > 0 and frame_count >= args.max_frames:
            break

    print(f"frames_shown: {frame_count}")
    cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
