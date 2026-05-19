#pragma once

#include <opencv2/core/mat.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace da3client {

struct FramePose {
  float qx = 0.0f;
  float qy = 0.0f;
  float qz = 0.0f;
  float qw = 1.0f;
  float tx = 0.0f;
  float ty = 0.0f;
  float tz = 0.0f;
};

struct Keyframe {
  int idx = 0;
  uint64_t timestamp_ns = 0;
  cv::Mat image_bgr;
  FramePose pose;
};

struct Da3Output {
  cv::Mat depth_vis;
  std::string scale_text = "scale: n/a (pose-translation)";
  std::string pair_label;
};

class Da3OnnxRunner {
 public:
  Da3OnnxRunner(const std::string& model_path, int input_width, int input_height);
  ~Da3OnnxRunner();

  Da3OnnxRunner(const Da3OnnxRunner&) = delete;
  Da3OnnxRunner& operator=(const Da3OnnxRunner&) = delete;

  bool IsReady() const;
  const std::string& ErrorMessage() const;

  bool InferPair(const Keyframe& a, const Keyframe& b, Da3Output* output);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace da3client
