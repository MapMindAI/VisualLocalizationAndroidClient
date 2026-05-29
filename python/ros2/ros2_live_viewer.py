#!/usr/bin/env python3
import argparse
import threading
import time
from dataclasses import dataclass
from typing import Optional

import cv2  # type: ignore
import numpy as np  # type: ignore
import rclpy  # type: ignore
from geometry_msgs.msg import PoseStamped  # type: ignore
from rclpy.node import Node  # type: ignore
from rclpy.qos import QoSDurabilityPolicy, QoSHistoryPolicy, QoSProfile, QoSReliabilityPolicy  # type: ignore
from sensor_msgs.msg import CompressedImage, Image as RosImage  # type: ignore
from utils import decode_compressed_image_rgb, depth_msg_to_bgr_turbo, draw_traj_canvas, resize_nearest, rot90_ccw


@dataclass
class PoseState:
    x: float
    y: float
    z: float


class LiveViewerNode(Node):
    def __init__(self, color_topic: str, depth_topic: str, vio_topic: str):
        super().__init__("vlp_live_viewer")
        qos_sensor = QoSProfile(
            history=QoSHistoryPolicy.KEEP_LAST,
            depth=10,
            reliability=QoSReliabilityPolicy.BEST_EFFORT,
            durability=QoSDurabilityPolicy.VOLATILE,
        )
        self.lock = threading.Lock()
        self.latest_rgb: Optional[np.ndarray] = None
        self.latest_depth: np.ndarray = np.zeros((480, 640, 3), dtype=np.uint8)
        self.latest_pose: Optional[PoseState] = None
        self.frame_count = 0

        self.traj_x = []
        self.traj_y = []
        self.traj_z = []

        self.create_subscription(CompressedImage, color_topic, self._on_color, qos_sensor)
        self.create_subscription(RosImage, depth_topic, self._on_depth, qos_sensor)
        self.create_subscription(PoseStamped, vio_topic, self._on_vio, qos_sensor)

    def _on_color(self, msg: CompressedImage):
        rgb = decode_compressed_image_rgb(bytes(msg.data))
        if rgb is None:
            return
        rgb = rot90_ccw(rgb)
        with self.lock:
            self.latest_rgb = rgb
            self.frame_count += 1

    def _on_depth(self, msg: RosImage):
        depth_rgb = depth_msg_to_bgr_turbo(bytes(msg.data), int(msg.width), int(msg.height), str(msg.encoding))
        if depth_rgb is None:
            return
        depth_rgb = rot90_ccw(depth_rgb)
        with self.lock:
            self.latest_depth = depth_rgb

    def _on_vio(self, msg: PoseStamped):
        p = msg.pose.position
        pose = PoseState(x=float(p.x), y=float(p.y), z=float(p.z))
        with self.lock:
            self.latest_pose = pose
            self.traj_x.append(pose.x)
            self.traj_y.append(pose.y)
            self.traj_z.append(-pose.z)


def main() -> int:
    ap = argparse.ArgumentParser(description="ROS2 live viewer: color + depth + vio_20hz trajectory")
    ap.add_argument("--color-topic", default="/camera/camera/color/image_rect_raw/compressed")
    ap.add_argument("--depth-topic", default="/camera/camera/depth/image_rect_raw")
    ap.add_argument("--vio-topic", default="/camera/camera/vio_20hz")
    ap.add_argument("--fps", type=float, default=30.0, help="UI refresh rate")
    args = ap.parse_args()

    if args.fps <= 0:
        raise ValueError("--fps must be > 0")

    rclpy.init()
    node = LiveViewerNode(args.color_topic, args.depth_topic, args.vio_topic)

    spin_thread = threading.Thread(target=rclpy.spin, args=(node,), daemon=True)
    spin_thread.start()

    control = {"quit": False, "paused": False}
    last_frame_count = -1
    frame_period = 1.0 / args.fps
    window_name = "ROS2 Live Viewer"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)

    try:
        while rclpy.ok() and not control["quit"]:
            t0 = time.time()
            with node.lock:
                rgb = None if node.latest_rgb is None else node.latest_rgb.copy()
                depth = node.latest_depth.copy()
                pose = node.latest_pose
                frame_count = node.frame_count
                traj_x = list(node.traj_x)
                traj_y = list(node.traj_y)
                traj_z = list(node.traj_z)

            if rgb is not None:
                if depth.shape[0] != rgb.shape[0] or depth.shape[1] != rgb.shape[1]:
                    depth_show = resize_nearest(depth, rgb.shape[1], rgb.shape[0])
                else:
                    depth_show = depth
                traj = draw_traj_canvas(traj_x, traj_z, rgb.shape[1], rgb.shape[0])
                rgb_bgr = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
                panel = np.hstack([rgb_bgr, depth_show, traj])
                if pose is not None:
                    txt = f"idx={frame_count-1} vio=({pose.x:.3f},{pose.y:.3f},{pose.z:.3f}) keys: space pause, q quit"
                else:
                    txt = f"idx={frame_count-1} keys: space pause, q quit"
                cv2.putText(panel, txt, (12, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2, cv2.LINE_AA)
                cv2.imshow(window_name, panel)
                last_frame_count = frame_count

            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                control["quit"] = True
            elif key == ord(" "):
                control["paused"] = not control["paused"]
            if control["paused"]:
                time.sleep(0.03)
                continue

            dt = time.time() - t0
            if dt < frame_period:
                time.sleep(frame_period - dt)
    finally:
        cv2.destroyAllWindows()
        node.destroy_node()
        rclpy.shutdown()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
