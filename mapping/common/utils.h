#pragma once

#include "mapping/common/pose_types.h"

#include <grpcpp/support/byte_buffer.h>
#include <sophus/se3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace vlputil {

Sophus::SE3d PoseToSE3d(const Sophus::SE3f& p);

uint32_t ReadU32LE(const uint8_t* p);
uint64_t ReadU64LE(const uint8_t* p);
float ReadF32LE(const uint8_t* p);

double TransDelta(const Sophus::SE3f& a, const Sophus::SE3f& b);
double QuatAngleDeg(const Sophus::SE3f& a, const Sophus::SE3f& b);

std::string ByteBufferToString(grpc::ByteBuffer* buffer);
std::string Base64Encode(const uint8_t* data, size_t len);
std::string JsonEscape(const std::string& s);

}  // namespace vlputil
