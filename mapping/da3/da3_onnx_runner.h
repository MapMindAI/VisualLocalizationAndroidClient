#pragma once

#include "mapping/common/pose_types.h"

#include <opencv2/core/mat.hpp>

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace da3client {
using mapping::Pose;

struct Keyframe {
  int idx = 0;
  uint64_t timestamp_ns = 0;
  cv::Mat image_bgr;
  Pose pose;
  float fx = 0.0f;
  float fy = 0.0f;
  float cx = 0.0f;
  float cy = 0.0f;
};

struct Da3Output {
  cv::Mat depth_vis;
  cv::Mat depth_metric;
  cv::Mat depth_a_rel;
  cv::Mat depth_b_rel;
  std::string scale_text = "scale: n/a (pose-translation)";
  std::string pair_label;
  int kf_a_idx = 0;
  int kf_b_idx = 0;
  Pose pose;
  float fx = 0.0f;
  float fy = 0.0f;
  float cx = 0.0f;
  float cy = 0.0f;
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

class Da3Worker {
 public:
  explicit Da3Worker(std::unique_ptr<Da3OnnxRunner> runner);
  ~Da3Worker();

  Da3Worker(const Da3Worker&) = delete;
  Da3Worker& operator=(const Da3Worker&) = delete;

  bool IsReady() const;
  std::string ErrorMessage() const;
  void Submit(const Keyframe& a, const Keyframe& b);
  std::optional<Da3Output> GetLatestOutput() const;
  std::string GetLatestStatus() const;

 private:
  struct Job {
    Keyframe a;
    Keyframe b;
  };

  void ThreadMain();

  std::unique_ptr<Da3OnnxRunner> runner_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  struct OverlapCache {
    int kf_b_idx = 0;
    cv::Mat depth_b_rel;
  };

  std::optional<Da3Output> latest_output_;
  std::optional<OverlapCache> last_pair_cache_;
  double chained_world_scale_ = 1.0;
  std::string last_status_;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace da3client
