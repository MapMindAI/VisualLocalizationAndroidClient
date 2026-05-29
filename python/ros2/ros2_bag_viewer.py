#!/usr/bin/env python3
import argparse
import os
import time
from dataclasses import dataclass
from typing import Optional

import numpy as np  # type: ignore
import rosbag2_py  # type: ignore
from rclpy.serialization import deserialize_message  # type: ignore
from rosidl_runtime_py.utilities import get_message  # type: ignore
from utils import (
    decode_compressed_image_rgb,
    depth_msg_to_bgr_turbo,
    draw_header_text,
    draw_traj_canvas,
    ensure_size_nearest,
    make_triptych_panel,
    rot90_ccw,
)


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
    ap.add_argument("--vio-topic", default="/camera/camera/vio_100hz")
    ap.add_argument("--speed", type=float, default=1.0, help="Playback speed multiplier")
    ap.add_argument("--max-frames", type=int, default=0)
    ap.add_argument("--save-video", default="", help="Optional output video path; when set, frames are written instead of shown")
    ap.add_argument("--video-fps", type=float, default=30.0, help="Output video FPS for --save-video")
    ap.add_argument("--log-every", type=int, default=60, help="Print progress every N rendered frames (0 to disable)")
    args = ap.parse_args()
    if args.speed <= 0:
        raise ValueError("--speed must be > 0")
    if args.video_fps <= 0:
        raise ValueError("--video-fps must be > 0")

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
    do_show = not bool(args.save_video)
    if do_show:
        cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    writer = None

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
            traj_z.append(latest_pose.z)
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
        depth_show = ensure_size_nearest(depth_show, rgb.shape[1], rgb.shape[0])

        traj = draw_traj_canvas(traj_x, traj_z, rgb.shape[1], rgb.shape[0])
        rgb_bgr = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
        panel = make_triptych_panel(rgb_bgr, depth_show, traj)
        if latest_pose is not None:
            txt = f"idx={frame_count-1} rel={rel_ns}ns vio=({latest_pose.x:.3f},{latest_pose.y:.3f},{latest_pose.z:.3f}) keys: space pause, q quit"
        else:
            txt = f"idx={frame_count-1} rel={rel_ns}ns keys: space pause, q quit"
        draw_header_text(panel, txt)
        if writer is None and args.save_video:
            os.makedirs(os.path.dirname(args.save_video) or ".", exist_ok=True)
            fourcc = cv2.VideoWriter_fourcc(*"mp4v")
            writer = cv2.VideoWriter(args.save_video, fourcc, float(args.video_fps), (panel.shape[1], panel.shape[0]))
            if not writer.isOpened():
                raise RuntimeError(f"Failed to open video writer: {args.save_video}")
        if writer is not None:
            writer.write(panel)
        elif do_show:
            cv2.imshow(window_name, panel)

        if args.log_every > 0 and (frame_count == 1 or frame_count % args.log_every == 0):
            mode = "save-video" if writer is not None else "display"
            print(
                f"[{frame_count:06d}] rel={rel_ns}ns mode={mode} "
                f"depth={'yes' if latest_depth_rgb is not None else 'no'} "
                f"traj_pts={len(traj_x)}"
            )

        target_elapsed = rel_ns / 1e9 / args.speed
        target_wall = start_wall + paused_accum_sec + target_elapsed
        if do_show:
            while time.time() < target_wall:
                key = cv2.waitKey(1) & 0xFF
                if key == ord("q"):
                    print("Quit requested by user.")
                    if writer is not None:
                        writer.release()
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
                        if writer is not None:
                            writer.release()
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
    if writer is not None:
        writer.release()
        print(f"video_saved: {args.save_video}")
    cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
