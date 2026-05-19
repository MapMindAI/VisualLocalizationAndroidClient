#include "utils.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vlputil {

Sophus::SE3d PoseToSE3(const da3client::FramePose& p) {
  Eigen::Quaterniond q(static_cast<double>(p.qw), static_cast<double>(p.qx),
                       static_cast<double>(p.qy), static_cast<double>(p.qz));
  q.normalize();
  Eigen::Vector3d t(static_cast<double>(p.tx), static_cast<double>(p.ty),
                    static_cast<double>(p.tz));
  return Sophus::SE3d(q, t);
}

da3client::FramePose SE3ToPose(const Sophus::SE3d& T) {
  const Eigen::Quaterniond q = T.unit_quaternion();
  const Eigen::Vector3d t = T.translation();
  da3client::FramePose p;
  p.qx = static_cast<float>(q.x());
  p.qy = static_cast<float>(q.y());
  p.qz = static_cast<float>(q.z());
  p.qw = static_cast<float>(q.w());
  p.tx = static_cast<float>(t.x());
  p.ty = static_cast<float>(t.y());
  p.tz = static_cast<float>(t.z());
  return p;
}

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64LE(const uint8_t* p) {
  return static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) |
         (static_cast<uint64_t>(p[2]) << 16) | (static_cast<uint64_t>(p[3]) << 24) |
         (static_cast<uint64_t>(p[4]) << 32) | (static_cast<uint64_t>(p[5]) << 40) |
         (static_cast<uint64_t>(p[6]) << 48) | (static_cast<uint64_t>(p[7]) << 56);
}

float ReadF32LE(const uint8_t* p) {
  const uint32_t bits = ReadU32LE(p);
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(float));
  return out;
}

double TransDelta(const da3client::FramePose& a, const da3client::FramePose& b) {
  const double dx = static_cast<double>(b.tx) - static_cast<double>(a.tx);
  const double dy = static_cast<double>(b.ty) - static_cast<double>(a.ty);
  const double dz = static_cast<double>(b.tz) - static_cast<double>(a.tz);
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double QuatAngleDeg(const da3client::FramePose& a, const da3client::FramePose& b) {
  std::array<double, 4> q1 = {
      static_cast<double>(a.qx), static_cast<double>(a.qy),
      static_cast<double>(a.qz), static_cast<double>(a.qw)};
  std::array<double, 4> q2 = {
      static_cast<double>(b.qx), static_cast<double>(b.qy),
      static_cast<double>(b.qz), static_cast<double>(b.qw)};
  auto norm4 = [](const std::array<double, 4>& q) {
    return std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  };
  const double n1 = norm4(q1);
  const double n2 = norm4(q2);
  if (n1 <= 1e-12 || n2 <= 1e-12) {
    return 0.0;
  }
  for (int i = 0; i < 4; ++i) {
    q1[i] /= n1;
    q2[i] /= n2;
  }
  const double dot = std::clamp(
      std::abs(q1[0] * q2[0] + q1[1] * q2[1] + q1[2] * q2[2] + q1[3] * q2[3]),
      -1.0, 1.0);
  const double angle_rad = 2.0 * std::acos(dot);
  return angle_rad * 180.0 / M_PI;
}

std::string ByteBufferToString(grpc::ByteBuffer* buffer) {
  std::vector<grpc::Slice> slices;
  const grpc::Status dump_status = buffer->Dump(&slices);
  if (!dump_status.ok()) {
    return {};
  }
  size_t total = 0;
  for (const auto& slice : slices) {
    total += slice.size();
  }
  std::string out;
  out.reserve(total);
  for (const auto& slice : slices) {
    out.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
  }
  return out;
}

std::string Base64Encode(const uint8_t* data, size_t len) {
  static constexpr char kB64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 2 < len; i += 3) {
    const uint32_t n =
        (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) |
        static_cast<uint32_t>(data[i + 2]);
    out.push_back(kB64[(n >> 18) & 0x3F]);
    out.push_back(kB64[(n >> 12) & 0x3F]);
    out.push_back(kB64[(n >> 6) & 0x3F]);
    out.push_back(kB64[n & 0x3F]);
  }
  if (i < len) {
    const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out.push_back(kB64[(n >> 18) & 0x3F]);
    if (i + 1 < len) {
      const uint32_t n2 = n | (static_cast<uint32_t>(data[i + 1]) << 8);
      out.push_back(kB64[(n2 >> 12) & 0x3F]);
      out.push_back(kB64[(n2 >> 6) & 0x3F]);
      out.push_back('=');
    } else {
      out.push_back(kB64[(n >> 12) & 0x3F]);
      out.push_back('=');
      out.push_back('=');
    }
  }
  return out;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace vlputil
