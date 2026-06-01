#pragma once

#include "mapping/common/pose_types.h"

#include <sophus/se3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <Eigen/Core>
#include <optional>
#include <string>

namespace vlputil {

Sophus::SE3d PoseToSE3d(const Sophus::SE3f& p);

uint32_t ReadU32LE(const uint8_t* p);
uint64_t ReadU64LE(const uint8_t* p);
float ReadF32LE(const uint8_t* p);

double TransDelta(const Sophus::SE3f& a, const Sophus::SE3f& b);
double QuatAngleDeg(const Sophus::SE3f& a, const Sophus::SE3f& b);

std::string Base64Encode(const uint8_t* data, size_t len);
std::string JsonEscape(const std::string& s);

// Returns Jet colormap RGB in [0, 255].
inline Eigen::Vector3f MakeJetColorRgb(float id) {
  if (id <= 0.0f) return Eigen::Vector3f(0.0f, 0.0f, 128.0f);
  if (id >= 1.0f) return Eigen::Vector3f(128.0f, 0.0f, 0.0f);

  int var_lvl = (id * 8);
  float tmp = (id * 8) - var_lvl;

  // OpenCV's Scalar ordering is B,G,R. Here we return RGB directly.
  if (var_lvl == 0) return Eigen::Vector3f(0.0f, 0.0f, 255.0f * (0.5f + 0.5f * tmp));
  if (var_lvl == 1) return Eigen::Vector3f(0.0f, 255.0f * (0.5f * tmp), 255.0f);
  if (var_lvl == 2) return Eigen::Vector3f(0.0f, 255.0f * (0.5f + 0.5f * tmp), 255.0f);
  if (var_lvl == 3) {
    return Eigen::Vector3f(255.0f * (0.5f * tmp), 255.0f, 255.0f * (1.0f - 0.5f * tmp));
  }
  if (var_lvl == 4) {
    return Eigen::Vector3f(255.0f * (0.5f + 0.5f * tmp), 255.0f, 255.0f * (0.5f - 0.5f * tmp));
  }
  if (var_lvl == 5) return Eigen::Vector3f(255.0f, 255.0f * (1.0f - 0.5f * tmp), 0.0f);
  if (var_lvl == 6) return Eigen::Vector3f(255.0f, 255.0f * (0.5f - 0.5f * tmp), 0.0f);
  if (var_lvl == 7) return Eigen::Vector3f(255.0f * (1.0f - 0.5f * tmp), 0.0f, 0.0f);
  return Eigen::Vector3f(255.0f, 255.0f, 255.0f);
}

}  // namespace vlputil
