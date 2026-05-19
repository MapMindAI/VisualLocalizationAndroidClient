#pragma once

#include "da3_onnx_runner.h"

#include <grpcpp/support/byte_buffer.h>
#include <sophus/se3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace vlputil {

Sophus::SE3d PoseToSE3(const da3client::FramePose& p);
da3client::FramePose SE3ToPose(const Sophus::SE3d& T);

uint32_t ReadU32LE(const uint8_t* p);
uint64_t ReadU64LE(const uint8_t* p);
float ReadF32LE(const uint8_t* p);

double TransDelta(const da3client::FramePose& a, const da3client::FramePose& b);
double QuatAngleDeg(const da3client::FramePose& a, const da3client::FramePose& b);

std::string ByteBufferToString(grpc::ByteBuffer* buffer);
std::string Base64Encode(const uint8_t* data, size_t len);
std::string JsonEscape(const std::string& s);

}  // namespace vlputil
