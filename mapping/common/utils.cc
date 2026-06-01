#include "mapping/common/utils.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vlputil {

Sophus::SE3d PoseToSE3d(const Sophus::SE3f& p) {
  return p.cast<double>();
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

double TransDelta(const Sophus::SE3f& a, const Sophus::SE3f& b) {
  const Eigen::Vector3f dt = b.translation() - a.translation();
  const double dx = static_cast<double>(dt.x());
  const double dy = static_cast<double>(dt.y());
  const double dz = static_cast<double>(dt.z());
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double QuatAngleDeg(const Sophus::SE3f& a, const Sophus::SE3f& b) {
  const Eigen::Quaternionf qa = a.unit_quaternion();
  const Eigen::Quaternionf qb = b.unit_quaternion();
  std::array<double, 4> q1 = {
      static_cast<double>(qa.x()), static_cast<double>(qa.y()),
      static_cast<double>(qa.z()), static_cast<double>(qa.w())};
  std::array<double, 4> q2 = {
      static_cast<double>(qb.x()), static_cast<double>(qb.y()),
      static_cast<double>(qb.z()), static_cast<double>(qb.w())};
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
