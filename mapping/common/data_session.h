#pragma once

#include "mapping/common/frame_protocol.h"

#include <opencv2/core/mat.hpp>

#include <cstdint>
#include <fstream>
#include <string>

namespace mapping {

struct DataSessionFrame {
  int64_t rel_ns = 0;
  vlpstream::FramePacket packet;
  cv::Mat depth_m;
  bool has_depth_intrinsics = false;
  float depth_fx = 0.0f;
  float depth_fy = 0.0f;
  float depth_cx = 0.0f;
  float depth_cy = 0.0f;
};

class DataSessionReader {
 public:
  explicit DataSessionReader(std::string path);

  bool Open();
  bool Next(DataSessionFrame* frame);

 private:
  bool ExtractDepthFromPayload(const std::string& payload, cv::Mat* depth_m) const;

  std::string path_;
  std::ifstream ifs_;
  std::string payload_buffer_;
};

}  // namespace mapping
