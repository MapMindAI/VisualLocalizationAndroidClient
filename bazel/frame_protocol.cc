#include "frame_protocol.h"

#include "utils.h"

#include <Eigen/Geometry>
#include <sophus/se3.hpp>

namespace vlpstream {
namespace {

constexpr uint32_t kFrameMagic = 0x564C5032U;  // "VLP2"
constexpr size_t kFrameHeaderSize = 64;

}  // namespace

std::optional<FramePacket> ParseFramePayload(const std::string& payload) {
  if (payload.size() < kFrameHeaderSize) {
    return std::nullopt;
  }
  const auto* data = reinterpret_cast<const uint8_t*>(payload.data());
  const uint32_t magic = vlputil::ReadU32LE(data + 0);
  if (magic != kFrameMagic) {
    return std::nullopt;
  }

  FramePacket packet;
  packet.timestamp_ns = vlputil::ReadU64LE(data + 4);
  packet.width = vlputil::ReadU32LE(data + 12);
  packet.height = vlputil::ReadU32LE(data + 16);
  packet.fx = vlputil::ReadF32LE(data + 20);
  packet.fy = vlputil::ReadF32LE(data + 24);
  packet.cx = vlputil::ReadF32LE(data + 28);
  packet.cy = vlputil::ReadF32LE(data + 32);
  packet.pose.qx = vlputil::ReadF32LE(data + 36);
  packet.pose.qy = vlputil::ReadF32LE(data + 40);
  packet.pose.qz = vlputil::ReadF32LE(data + 44);
  packet.pose.qw = vlputil::ReadF32LE(data + 48);
  packet.pose.tx = vlputil::ReadF32LE(data + 52);
  packet.pose.ty = vlputil::ReadF32LE(data + 56);
  packet.pose.tz = vlputil::ReadF32LE(data + 60);

  const Sophus::SE3d kTransformCameraToOpenGLDevice(
      Eigen::Quaterniond(0, 1, 0, 0), Eigen::Vector3d::Zero());
  packet.pose = vlputil::SE3ToPose(vlputil::PoseToSE3(packet.pose) * kTransformCameraToOpenGLDevice);

  packet.jpeg_bytes.assign(data + kFrameHeaderSize, data + payload.size());
  if (packet.jpeg_bytes.empty()) {
    return std::nullopt;
  }
  return packet;
}

}  // namespace vlpstream
