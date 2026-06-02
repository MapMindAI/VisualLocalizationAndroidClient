#!/usr/bin/env python3
import argparse
import os
import shutil
import sys
from typing import Optional

import rosbag2_py  # type: ignore
from geometry_msgs.msg import PoseStamped  # type: ignore
from rclpy.serialization import serialize_message  # type: ignore
from sensor_msgs.msg import CameraInfo, CompressedImage, Image  # type: ignore
from std_msgs.msg import Header  # type: ignore

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
PYTHON_DIR = os.path.dirname(THIS_DIR)
if PYTHON_DIR not in sys.path:
    sys.path.insert(0, PYTHON_DIR)

from vlprec_reader import iter_vlprec  # type: ignore


def _make_topic_metadata(name: str, msg_type: str):
    kwargs = {
        "name": name,
        "type": msg_type,
        "serialization_format": "cdr",
    }
    try:
        return rosbag2_py.TopicMetadata(
            offered_qos_profiles="",
            **kwargs,
        )
    except TypeError:
        try:
            return rosbag2_py.TopicMetadata(**kwargs)
        except TypeError:
            meta = rosbag2_py.TopicMetadata()
            meta.name = name
            meta.type = msg_type
            meta.serialization_format = "cdr"
            if hasattr(meta, "offered_qos_profiles"):
                meta.offered_qos_profiles = ""
            return meta


def _ns_to_stamp(timestamp_ns: int):
    sec = int(timestamp_ns // 1_000_000_000)
    nanosec = int(timestamp_ns % 1_000_000_000)
    return sec, nanosec


def _make_header(timestamp_ns: int, frame_id: str) -> Header:
    header = Header()
    sec, nanosec = _ns_to_stamp(timestamp_ns)
    header.stamp.sec = sec
    header.stamp.nanosec = nanosec
    header.frame_id = frame_id
    return header


def _make_color_msg(timestamp_ns: int, frame_id: str, jpeg_bytes: bytes) -> CompressedImage:
    msg = CompressedImage()
    msg.header = _make_header(timestamp_ns, frame_id)
    msg.format = "jpeg"
    msg.data = jpeg_bytes
    return msg


def _make_depth_msg(timestamp_ns: int, frame_id: str, width: int, height: int, depth_bytes: bytes) -> Image:
    msg = Image()
    msg.header = _make_header(timestamp_ns, frame_id)
    msg.height = int(height)
    msg.width = int(width)
    msg.encoding = "mono16"
    msg.is_bigendian = 0
    msg.step = int(width) * 2
    msg.data = depth_bytes
    return msg


def _make_pose_msg(timestamp_ns: int, frame_id: str, rec) -> PoseStamped:
    msg = PoseStamped()
    msg.header = _make_header(timestamp_ns, frame_id)
    msg.pose.position.x = float(rec.tx)
    msg.pose.position.y = float(rec.ty)
    msg.pose.position.z = float(rec.tz)
    msg.pose.orientation.x = float(rec.qx)
    msg.pose.orientation.y = float(rec.qy)
    msg.pose.orientation.z = float(rec.qz)
    msg.pose.orientation.w = float(rec.qw)
    return msg


def _make_camera_info_msg(timestamp_ns: int, frame_id: str, width: int, height: int, rec) -> CameraInfo:
    msg = CameraInfo()
    msg.header = _make_header(timestamp_ns, frame_id)
    msg.width = int(width)
    msg.height = int(height)
    msg.distortion_model = "plumb_bob"
    msg.d = [0.0, 0.0, 0.0, 0.0, 0.0]
    msg.k = [
        float(rec.fx),
        0.0,
        float(rec.cx),
        0.0,
        float(rec.fy),
        float(rec.cy),
        0.0,
        0.0,
        1.0,
    ]
    msg.r = [
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    ]
    msg.p = [
        float(rec.fx),
        0.0,
        float(rec.cx),
        0.0,
        0.0,
        float(rec.fy),
        float(rec.cy),
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
    ]
    return msg


def _open_writer(output_dir: str, storage_id: str):
    writer = rosbag2_py.SequentialWriter()
    storage_options = rosbag2_py.StorageOptions(uri=output_dir, storage_id=storage_id)
    converter_options = rosbag2_py.ConverterOptions("cdr", "cdr")
    writer.open(storage_options, converter_options)
    return writer


def _safe_makedirs_for_output(output_dir: str, overwrite: bool) -> None:
    if os.path.exists(output_dir):
        if not overwrite:
            raise FileExistsError(f"Output already exists: {output_dir}")
        if not os.path.isdir(output_dir):
            raise FileExistsError(f"Output path exists and is not a directory: {output_dir}")
        shutil.rmtree(output_dir)
    os.makedirs(os.path.dirname(output_dir) or ".", exist_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="Convert VLPREC1 (.rec) into a ROS2 bag.")
    ap.add_argument("--input", required=True, help="Input .rec path")
    ap.add_argument("--output", required=True, help="Output rosbag directory")
    ap.add_argument("--storage-id", default="sqlite3", help="rosbag2 storage plugin")
    ap.add_argument("--overwrite", action="store_true", help="Replace output directory if it exists")
    ap.add_argument("--max-frames", type=int, default=0, help="Stop after N frames (0 = all)")
    ap.add_argument("--log-every", type=int, default=60, help="Print progress every N frames (0 disables)")
    ap.add_argument("--color-topic", default="/camera/camera/color/image_rect_raw/compressed")
    ap.add_argument("--depth-topic", default="/camera/camera/depth/image_rect_raw")
    ap.add_argument("--pose-topic", default="/camera/camera/vio")
    ap.add_argument("--color-info-topic", default="/camera/camera/color/camera_info")
    ap.add_argument("--color-frame-id", default="camera_color_optical_frame")
    ap.add_argument("--depth-frame-id", default="camera_depth_optical_frame")
    ap.add_argument("--pose-frame-id", default="map")
    args = ap.parse_args()

    _safe_makedirs_for_output(args.output, args.overwrite)
    writer = _open_writer(args.output, args.storage_id)

    writer.create_topic(
        _make_topic_metadata(args.color_topic, "sensor_msgs/msg/CompressedImage")
    )
    writer.create_topic(
        _make_topic_metadata(args.depth_topic, "sensor_msgs/msg/Image")
    )
    writer.create_topic(
        _make_topic_metadata(args.pose_topic, "geometry_msgs/msg/PoseStamped")
    )
    writer.create_topic(
        _make_topic_metadata(args.color_info_topic, "sensor_msgs/msg/CameraInfo")
    )

    written_color = 0
    written_depth = 0
    written_pose = 0
    written_info = 0
    skipped_depth = 0
    first_ts_ns: Optional[int] = None
    last_ts_ns: Optional[int] = None

    for rec in iter_vlprec(args.input):
        stamp_ns = int(rec.timestamp_ns)
        if first_ts_ns is None:
            first_ts_ns = stamp_ns
        last_ts_ns = stamp_ns

        color_msg = _make_color_msg(stamp_ns, args.color_frame_id, rec.body)
        writer.write(args.color_topic, serialize_message(color_msg), stamp_ns)
        written_color += 1

        pose_msg = _make_pose_msg(stamp_ns, args.pose_frame_id, rec)
        writer.write(args.pose_topic, serialize_message(pose_msg), stamp_ns)
        written_pose += 1

        info_msg = _make_camera_info_msg(stamp_ns, args.color_frame_id, rec.width, rec.height, rec)
        writer.write(args.color_info_topic, serialize_message(info_msg), stamp_ns)
        written_info += 1

        expected_depth_bytes = int(rec.depth_w) * int(rec.depth_h) * 2
        if rec.depth_w > 0 and rec.depth_h > 0 and len(rec.depth_bytes) >= expected_depth_bytes:
            depth_msg = _make_depth_msg(
                stamp_ns,
                args.depth_frame_id,
                rec.depth_w,
                rec.depth_h,
                rec.depth_bytes[:expected_depth_bytes],
            )
            writer.write(args.depth_topic, serialize_message(depth_msg), stamp_ns)
            written_depth += 1
        else:
            skipped_depth += 1

        total_frames = written_color
        if args.log_every > 0 and (total_frames == 1 or total_frames % args.log_every == 0):
            print(
                f"[{total_frames:06d}] ts={stamp_ns} "
                f"color={rec.width}x{rec.height} depth={rec.depth_w}x{rec.depth_h} "
                f"intr=({rec.fx:.4f},{rec.fy:.4f},{rec.cx:.4f},{rec.cy:.4f})"
            )

        if args.max_frames > 0 and total_frames >= args.max_frames:
            break

    print("----")
    print(f"input: {args.input}")
    print(f"output: {args.output}")
    print(f"written_color: {written_color}")
    print(f"written_depth: {written_depth}")
    print(f"written_pose: {written_pose}")
    print(f"written_color_info: {written_info}")
    print(f"skipped_depth: {skipped_depth}")
    if first_ts_ns is not None and last_ts_ns is not None:
        print(f"first_ts_ns: {first_ts_ns}")
        print(f"last_ts_ns: {last_ts_ns}")
        print(f"duration_sec: {(last_ts_ns - first_ts_ns) / 1e9:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
