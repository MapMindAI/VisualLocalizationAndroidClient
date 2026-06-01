#!/usr/bin/env bash
set -euo pipefail

MAIN_CPU_CORE="${MAIN_CPU_CORE:-6,7}"
VIEWER_CPU_CORE="${VIEWER_CPU_CORE:-5}"

TOPIC_RGB="${TOPIC_RGB:-/camera/camera/color/image_rect_raw/compressed}"
TOPIC_DEPTH="${TOPIC_DEPTH:-/camera/camera/depth/image_rect_raw}"
TOPIC_ESDF_CLOUD="${TOPIC_ESDF_CLOUD:-/vlp/esdf_cloud}"
TOPIC_POSE="${TOPIC_POSE:-/camera/camera/vio_100hz}"
ROSBAG_PATH="${ROSBAG_PATH:-/VisualLocalizationAndroidClient/data/outdoor_large_circle/rosbag}"
ROSBAG_RATE="${ROSBAG_RATE:-1.0}"
ROSBAG_LOOP="${ROSBAG_LOOP:-0}"

cleanup() {
  local code=$?
  trap - INT TERM EXIT
  if [[ -n "${MAIN_PID:-}" ]]; then kill "${MAIN_PID}" >/dev/null 2>&1 || true; fi
  if [[ -n "${VIEWER_PID:-}" ]]; then kill "${VIEWER_PID}" >/dev/null 2>&1 || true; fi
  if [[ -n "${BAG_PID:-}" ]]; then kill "${BAG_PID}" >/dev/null 2>&1 || true; fi
  wait || true
  exit "$code"
}
trap cleanup INT TERM EXIT

bazel build \
  //mapping/rosnodes:voxblox_ros_node \
  //mapping/rosnodes:pangolin_visualizer_node

MAIN_BIN="bazel-bin/mapping/rosnodes/voxblox_ros_node"
VIEWER_BIN="bazel-bin/mapping/rosnodes/pangolin_visualizer_node"

taskset -c "${MAIN_CPU_CORE}" "${MAIN_BIN}" \
  --logtostderr=1 --topic_depth="${TOPIC_DEPTH}" \
  --topic_pose="${TOPIC_POSE}" \
  --topic_esdf="${TOPIC_ESDF_CLOUD}" \
  "$@" &
MAIN_PID=$!

taskset -c "${VIEWER_CPU_CORE}" "${VIEWER_BIN}" \
  --logtostderr=1 --topic_rgb_compressed="${TOPIC_RGB}" \
  --topic_depth="${TOPIC_DEPTH}" \
  --topic_pose="${TOPIC_POSE}" \
  --topic_esdf="${TOPIC_ESDF_CLOUD}" &
VIEWER_PID=$!

if [[ -n "${ROSBAG_PATH}" ]]; then
  BAG_ARGS=(play "${ROSBAG_PATH}" --clock --rate "${ROSBAG_RATE}")
  if [[ "${ROSBAG_LOOP}" == "1" ]]; then
    BAG_ARGS+=(--loop)
  fi
  ros2 bag "${BAG_ARGS[@]}" &
  BAG_PID=$!
fi

if [[ -n "${BAG_PID:-}" ]]; then
  wait -n "${MAIN_PID}" "${VIEWER_PID}" "${BAG_PID}" || true
else
  wait -n "${MAIN_PID}" "${VIEWER_PID}" || true
fi
cleanup
