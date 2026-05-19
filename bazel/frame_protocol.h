#pragma once

#include "da3_onnx_runner.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vlpstream {

struct FramePacket {
  uint64_t timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  float fx = 0.0f;
  float fy = 0.0f;
  float cx = 0.0f;
  float cy = 0.0f;
  da3client::FramePose pose;
  std::vector<uint8_t> jpeg_bytes;
};

std::optional<FramePacket> ParseFramePayload(const std::string& payload);

}  // namespace vlpstream
