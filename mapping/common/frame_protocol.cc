#include "mapping/common/frame_protocol.h"

#include "mapping/common/utils.h"

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
  const float qx = vlputil::ReadF32LE(data + 36);
  const float qy = vlputil::ReadF32LE(data + 40);
  const float qz = vlputil::ReadF32LE(data + 44);
  const float qw = vlputil::ReadF32LE(data + 48);
  const float tx = vlputil::ReadF32LE(data + 52);
  const float ty = vlputil::ReadF32LE(data + 56);
  const float tz = vlputil::ReadF32LE(data + 60);
  Eigen::Quaternionf q(qw, qx, qy, qz);
  q.normalize();
  packet.pose = Sophus::SE3f(q, Eigen::Vector3f(tx, ty, tz));

  const Sophus::SE3f kTransformCameraToOpenGLDevice(
      Eigen::Quaternionf(0.0f, 1.0f, 0.0f, 0.0f), Eigen::Vector3f::Zero());
  packet.pose = packet.pose * kTransformCameraToOpenGLDevice;

  packet.jpeg_bytes.assign(data + kFrameHeaderSize, data + payload.size());
  if (packet.jpeg_bytes.empty()) {
    return std::nullopt;
  }
  return packet;
}

}  // namespace vlpstream
