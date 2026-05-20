#include "mapping/da3/da3_onnx_runner.h"

#include <Eigen/Geometry>
#include <glog/logging.h>
#include <onnxruntime_cxx_api.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace da3client {
namespace {

double TransDelta(const Pose& a, const Pose& b) {
  const Eigen::Vector3f dt = b.translation() - a.translation();
  const double dx = static_cast<double>(dt.x());
  const double dy = static_cast<double>(dt.y());
  const double dz = static_cast<double>(dt.z());
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

cv::Mat ColorizeDepth(const cv::Mat& depth_f32) {
  if (depth_f32.empty() || depth_f32.type() != CV_32F) {
    return {};
  }
  double mn = 0.0;
  double mx = 0.0;
  cv::minMaxLoc(depth_f32, &mn, &mx);
  if (!std::isfinite(mn) || !std::isfinite(mx) || mx <= mn) {
    return {};
  }
  cv::Mat norm;
  depth_f32.convertTo(norm, CV_32F, 1.0 / (mx - mn), -mn / (mx - mn));
  cv::Mat u8;
  norm.convertTo(u8, CV_8U, 255.0);
  cv::Mat vis;
  cv::applyColorMap(u8, vis, cv::COLORMAP_INFERNO);
  return vis;
}

std::string ShapeToString(const std::vector<int64_t>& shape) {
  std::string out = "[";
  for (size_t i = 0; i < shape.size(); ++i) {
    out += std::to_string(shape[i]);
    if (i + 1 < shape.size()) {
      out += ",";
    }
  }
  out += "]";
  return out;
}

cv::Mat NchwToHwcMat(const std::vector<float>& nchw, int h, int w) {
  const size_t plane = static_cast<size_t>(h) * w;
  if (nchw.size() != plane * 3) {
    return {};
  }
  cv::Mat c0(h, w, CV_32F, const_cast<float*>(nchw.data() + 0 * plane));
  cv::Mat c1(h, w, CV_32F, const_cast<float*>(nchw.data() + 1 * plane));
  cv::Mat c2(h, w, CV_32F, const_cast<float*>(nchw.data() + 2 * plane));
  std::vector<cv::Mat> channels = {c0, c1, c2};
  cv::Mat hwc;
  cv::merge(channels, hwc);
  return hwc;
}

std::optional<double> MeanRatioScale(const cv::Mat& ref_depth, const cv::Mat& query_depth,
                                     int step = 4) {
  if (ref_depth.empty() || query_depth.empty() || ref_depth.type() != CV_32F ||
      query_depth.type() != CV_32F) {
    return std::nullopt;
  }
  cv::Mat query_resized;
  if (query_depth.size() != ref_depth.size()) {
    cv::resize(query_depth, query_resized, ref_depth.size(), 0.0, 0.0, cv::INTER_LINEAR);
  } else {
    query_resized = query_depth;
  }

  std::vector<float> ratios;
  ratios.reserve(static_cast<size_t>(ref_depth.rows) * ref_depth.cols / 16);
  for (int y = 0; y < ref_depth.rows; y += step) {
    const float* ref_row = ref_depth.ptr<float>(y);
    const float* q_row = query_resized.ptr<float>(y);
    for (int x = 0; x < ref_depth.cols; x += step) {
      const float a = ref_row[x];
      const float b = q_row[x];
      if (!std::isfinite(a) || !std::isfinite(b) || a <= 1e-6f || b <= 1e-6f) {
        continue;
      }
      const float r = a / b;
      if (std::isfinite(r) && r > 1e-4f && r < 1e4f) {
        ratios.push_back(r);
      }
    }
  }
  if (ratios.size() < 64) {
    return std::nullopt;
  }
  double sum = 0.0;
  for (const float r : ratios) {
    sum += static_cast<double>(r);
  }
  const double mean = sum / static_cast<double>(ratios.size());
  if (!std::isfinite(mean) || mean <= 0.0) {
    return std::nullopt;
  }
  return mean;
}

}  // namespace

struct Da3OnnxRunner::Impl {
  Impl(const std::string& model_path_in, int input_width_in, int input_height_in)
      : env(ORT_LOGGING_LEVEL_WARNING, "da3"),
        model_path(model_path_in),
        input_width(input_width_in),
        input_height(input_height_in) {
    Init();
  }

  bool InferPair(const Keyframe& a, const Keyframe& b, Da3Output* output) {
    if (!ready || output == nullptr) {
      return false;
    }
    output->kf_a_idx = a.idx;
    output->kf_b_idx = b.idx;
    output->pair_label = cv::format("(kf%d,kf%d)", a.idx, b.idx);
    try {
      std::vector<float> a_nchw;
      std::vector<float> b_nchw;
      Preprocess(a.image_bgr, &a_nchw);
      Preprocess(b.image_bgr, &b_nchw);

      std::vector<Ort::Value> input_values;
      input_values.reserve(input_names.size());
      std::vector<std::vector<float>> owned_inputs;
      std::vector<std::vector<int64_t>> owned_dims;

      for (size_t i = 0; i < input_names.size(); ++i) {
        const std::vector<int64_t>& shape =
            (i < input_shapes.size()) ? input_shapes[i] : input_shapes.front();
        BuildOneInput(input_names[i], shape, a_nchw, b_nchw, &owned_inputs, &owned_dims);
        input_values.emplace_back(MakeTensor(owned_inputs.back(), owned_dims.back()));
      }

      const auto t0 = std::chrono::steady_clock::now();
      auto outputs = session->Run(Ort::RunOptions{nullptr}, input_name_ptrs.data(),
                                  input_values.data(), input_values.size(),
                                  output_name_ptrs.data(), output_name_ptrs.size());
      const auto t1 = std::chrono::steady_clock::now();
      const double infer_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

      std::unordered_map<std::string, size_t> out_idx;
      for (size_t i = 0; i < output_names.size(); ++i) {
        out_idx[output_names[i]] = i;
      }
      if (out_idx.find("depth") == out_idx.end()) {
        output->scale_text = "scale: failed (missing depth output)";
        return false;
      }

      cv::Mat depth_a_rel;
      cv::Mat depth_b_rel;
      if (!ExtractDepthChannel(outputs[out_idx["depth"]], 0, &depth_a_rel) ||
          !ExtractDepthChannel(outputs[out_idx["depth"]], 1, &depth_b_rel)) {
        output->scale_text = "scale: failed (invalid depth tensor)";
        return false;
      }
      cv::resize(depth_a_rel, depth_a_rel, a.image_bgr.size(), 0.0, 0.0, cv::INTER_LINEAR);
      cv::resize(depth_b_rel, depth_b_rel, b.image_bgr.size(), 0.0, 0.0, cv::INTER_LINEAR);

      output->depth_a_rel = std::move(depth_a_rel);
      output->depth_b_rel = std::move(depth_b_rel);
      output->depth_vis = ColorizeDepth(output->depth_b_rel);
      output->depth_metric = output->depth_b_rel.clone();
      output->pair_label = cv::format("(kf%d,kf%d) infer=%.1fms", a.idx, b.idx, infer_ms);
      output->pose = b.pose;
      output->fx = b.fx;
      output->fy = b.fy;
      output->cx = b.cx;
      output->cy = b.cy;
      return !output->depth_vis.empty();
    } catch (const std::exception& e) {
      output->scale_text = std::string("scale: failed (") + e.what() + ")";
      LOG(INFO) << output->scale_text;
      return false;
    }
  }

  void Init() {
    try {
      std::ifstream f(model_path, std::ios::binary);
      if (!f.good()) {
        error_msg = "Model file not found: " + model_path;
        return;
      }

      Ort::SessionOptions opts;
      opts.SetIntraOpNumThreads(2);
      opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      session = std::make_unique<Ort::Session>(env, model_path.c_str(), opts);

      Ort::AllocatorWithDefaultOptions allocator;
      const size_t in_count = session->GetInputCount();
      const size_t out_count = session->GetOutputCount();

      input_names.reserve(in_count);
      input_shapes.reserve(in_count);
      for (size_t i = 0; i < in_count; ++i) {
        Ort::AllocatedStringPtr name = session->GetInputNameAllocated(i, allocator);
        input_names.push_back(name.get());
        auto info = session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo();
        input_shapes.push_back(info.GetShape());
      }

      output_names.reserve(out_count);
      for (size_t i = 0; i < out_count; ++i) {
        Ort::AllocatedStringPtr name = session->GetOutputNameAllocated(i, allocator);
        output_names.push_back(name.get());
      }

      input_name_ptrs.reserve(input_names.size());
      for (const auto& name : input_names) {
        input_name_ptrs.push_back(name.c_str());
      }
      output_name_ptrs.reserve(output_names.size());
      for (const auto& name : output_names) {
        output_name_ptrs.push_back(name.c_str());
      }

      ready = true;
      std::string output_names_csv;
      for (size_t i = 0; i < output_names.size(); ++i) {
        output_names_csv += output_names[i];
        if (i + 1 < output_names.size()) {
          output_names_csv += ",";
        }
      }
      LOG(INFO) << "[DA3] loaded model: " << model_path << " inputs=" << input_names.size()
                << " outputs=" << output_names_csv;
      for (size_t i = 0; i < input_names.size(); ++i) {
        const std::vector<int64_t>& shape =
            (i < input_shapes.size()) ? input_shapes[i] : std::vector<int64_t>{};
        LOG(INFO) << "[DA3] input[" << i << "] name=" << input_names[i]
                  << " shape=" << ShapeToString(shape);
      }
    } catch (const std::exception& e) {
      error_msg = std::string("Failed to initialize DA3 model: ") + e.what();
    }
  }

  void Preprocess(const cv::Mat& image_bgr, std::vector<float>* nchw) const {
    cv::Mat rgb;
    cv::cvtColor(image_bgr, rgb, cv::COLOR_BGR2RGB);
    cv::Mat resized;
    cv::resize(rgb, resized, cv::Size(input_width, input_height), 0.0, 0.0, cv::INTER_LINEAR);
    cv::Mat resized_f32;
    resized.convertTo(resized_f32, CV_32FC3, 1.0 / 255.0);

    std::vector<cv::Mat> channels(3);
    cv::split(resized_f32, channels);

    const size_t plane = static_cast<size_t>(input_height) * input_width;
    nchw->resize(plane * 3);
    std::memcpy(nchw->data() + 0 * plane, channels[0].data, plane * sizeof(float));
    std::memcpy(nchw->data() + 1 * plane, channels[1].data, plane * sizeof(float));
    std::memcpy(nchw->data() + 2 * plane, channels[2].data, plane * sizeof(float));
  }

  void BuildOneInput(const std::string& input_name, const std::vector<int64_t>& shape,
                     const std::vector<float>& a_nchw,
                     const std::vector<float>& b_nchw,
                     std::vector<std::vector<float>>* owned_inputs,
                     std::vector<std::vector<int64_t>>* owned_dims) const {
    if (input_name == "image") {
      std::vector<float> input;
      input.reserve(a_nchw.size() + b_nchw.size());
      input.insert(input.end(), a_nchw.begin(), a_nchw.end());
      input.insert(input.end(), b_nchw.begin(), b_nchw.end());
      owned_inputs->push_back(std::move(input));
      owned_dims->push_back({1, 2, 3, input_height, input_width});
      return;
    }

    if (shape.size() == 5) {
      std::vector<float> input;
      input.reserve(a_nchw.size() + b_nchw.size());
      input.insert(input.end(), a_nchw.begin(), a_nchw.end());
      input.insert(input.end(), b_nchw.begin(), b_nchw.end());
      owned_inputs->push_back(std::move(input));
      owned_dims->push_back({1, 2, 3, input_height, input_width});
      return;
    }

    if (shape.size() == 4) {
      const int64_t c1 = shape[1];
      const int64_t c3 = shape[3];
      if (c1 == 6) {
        std::vector<float> input;
        input.reserve(a_nchw.size() + b_nchw.size());
        input.insert(input.end(), a_nchw.begin(), a_nchw.end());
        input.insert(input.end(), b_nchw.begin(), b_nchw.end());
        owned_inputs->push_back(std::move(input));
        owned_dims->push_back({1, 6, input_height, input_width});
        return;
      }
      if (c3 == 6) {
        const cv::Mat a_hwc = NchwToHwcMat(a_nchw, input_height, input_width);
        const cv::Mat b_hwc = NchwToHwcMat(b_nchw, input_height, input_width);
        if (a_hwc.empty() || b_hwc.empty()) {
          owned_inputs->push_back(a_nchw);
          owned_dims->push_back({1, 3, input_height, input_width});
          return;
        }
        std::vector<cv::Mat> ab_channels;
        cv::split(a_hwc, ab_channels);
        std::vector<cv::Mat> b_channels;
        cv::split(b_hwc, b_channels);
        ab_channels.insert(ab_channels.end(), b_channels.begin(), b_channels.end());
        cv::Mat ab_hwc;
        cv::merge(ab_channels, ab_hwc);

        std::vector<float> input(static_cast<size_t>(input_height) * input_width * 6);
        std::memcpy(input.data(), ab_hwc.data, input.size() * sizeof(float));
        owned_inputs->push_back(std::move(input));
        owned_dims->push_back({1, input_height, input_width, 6});
        return;
      }
    }

    owned_inputs->push_back(a_nchw);
    owned_dims->push_back({1, 3, input_height, input_width});
  }

  Ort::Value MakeTensor(std::vector<float>& data, const std::vector<int64_t>& dims) const {
    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    return Ort::Value::CreateTensor<float>(mem_info, data.data(), data.size(), dims.data(),
                                           dims.size());
  }

  bool ExtractDepthChannel(const Ort::Value& value, int preferred_channel,
                           cv::Mat* depth_out) const {
    if (!value.IsTensor() || depth_out == nullptr) {
      return false;
    }
    auto info = value.GetTensorTypeAndShapeInfo();
    std::vector<int64_t> shape = info.GetShape();
    const float* data = value.GetTensorData<float>();

    if (shape.size() == 4 && shape[0] >= 1 && shape[2] > 0 && shape[3] > 0) {
      const int h = static_cast<int>(shape[2]);
      const int w = static_cast<int>(shape[3]);
      const int c = static_cast<int>(shape[1]);
      const int channel_idx = std::max(0, std::min(c - 1, preferred_channel));
      const size_t offset = static_cast<size_t>(channel_idx) * h * w;
      cv::Mat depth(h, w, CV_32F);
      std::memcpy(depth.data, data + offset, static_cast<size_t>(h) * w * sizeof(float));
      *depth_out = depth;
      return true;
    }

    if (shape.size() == 3 && shape[0] >= 1 && shape[1] > 0 && shape[2] > 0) {
      const int h = static_cast<int>(shape[1]);
      const int w = static_cast<int>(shape[2]);
      cv::Mat depth(h, w, CV_32F);
      std::memcpy(depth.data, data, static_cast<size_t>(h) * w * sizeof(float));
      *depth_out = depth;
      return true;
    }

    if (shape.size() == 2 && shape[0] > 0 && shape[1] > 0) {
      const int h = static_cast<int>(shape[0]);
      const int w = static_cast<int>(shape[1]);
      cv::Mat depth(h, w, CV_32F);
      std::memcpy(depth.data, data, static_cast<size_t>(h) * w * sizeof(float));
      *depth_out = depth;
      return true;
    }

    return false;
  }

  std::optional<double> ComputeScaleFromExtrinsics(const Ort::Value& value, const Pose& pose1,
                                                   const Pose& pose2) const {
    if (!value.IsTensor()) {
      return std::nullopt;
    }
    auto info = value.GetTensorTypeAndShapeInfo();
    std::vector<int64_t> shape = info.GetShape();
    if (shape.size() != 4 || shape[0] < 1 || shape[1] < 2 || shape[2] != 3 || shape[3] != 4) {
      return std::nullopt;
    }

    const float* ex = value.GetTensorData<float>();
    auto at = [&](int f, int r, int c) -> double {
      const size_t idx = ((static_cast<size_t>(f) * 3 + r) * 4) + c;
      return static_cast<double>(ex[idx]);
    };

    const double t1x = at(0, 0, 3);
    const double t1y = at(0, 1, 3);
    const double t1z = at(0, 2, 3);
    const double t2x = at(1, 0, 3);
    const double t2y = at(1, 1, 3);
    const double t2z = at(1, 2, 3);

    const double dx = t2x - t1x;
    const double dy = t2y - t1y;
    const double dz = t2z - t1z;
    const double da3_delta = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(da3_delta) || da3_delta <= 1e-8) {
      return std::nullopt;
    }

    const double real_delta = TransDelta(pose1, pose2);
    if (!std::isfinite(real_delta) || real_delta <= 1e-8) {
      return std::nullopt;
    }
    return real_delta / da3_delta;
  }

  Ort::Env env;
  std::string model_path;
  int input_width = 392;
  int input_height = 224;
  bool ready = false;
  std::string error_msg;
  std::unique_ptr<Ort::Session> session;

  std::vector<std::string> input_names;
  std::vector<std::string> output_names;
  std::vector<const char*> input_name_ptrs;
  std::vector<const char*> output_name_ptrs;
  std::vector<std::vector<int64_t>> input_shapes;
};

Da3OnnxRunner::Da3OnnxRunner(const std::string& model_path, int input_width, int input_height)
    : impl_(std::make_unique<Impl>(model_path, input_width, input_height)) {}

Da3OnnxRunner::~Da3OnnxRunner() = default;

bool Da3OnnxRunner::IsReady() const { return impl_ && impl_->ready; }

const std::string& Da3OnnxRunner::ErrorMessage() const {
  static const std::string kEmpty;
  if (!impl_) {
    return kEmpty;
  }
  return impl_->error_msg;
}

bool Da3OnnxRunner::InferPair(const Keyframe& a, const Keyframe& b, Da3Output* output) {
  return impl_ && impl_->InferPair(a, b, output);
}

Da3Worker::Da3Worker(std::unique_ptr<Da3OnnxRunner> runner)
    : runner_(std::move(runner)), worker_(&Da3Worker::ThreadMain, this) {
  std::lock_guard<std::mutex> lock(mu_);
  last_status_ = "DA3: waiting for first keyframe pair";
}

Da3Worker::~Da3Worker() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    jobs_.clear();
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

bool Da3Worker::IsReady() const { return runner_ != nullptr && runner_->IsReady(); }

std::string Da3Worker::ErrorMessage() const {
  if (!runner_) {
    return "DA3 runner missing";
  }
  return runner_->ErrorMessage();
}

void Da3Worker::Submit(const Keyframe& a, const Keyframe& b) {
  std::lock_guard<std::mutex> lock(mu_);
  jobs_.clear();
  jobs_.push_back({a, b});
  last_status_ = cv::format("DA3: queued (kf%d,kf%d)", a.idx, b.idx);
  cv_.notify_one();
}

std::optional<Da3Output> Da3Worker::GetLatestOutput() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!latest_output_.has_value()) {
    return std::nullopt;
  }
  Da3Output out;
  out.scale_text = latest_output_->scale_text;
  out.pair_label = latest_output_->pair_label;
  out.depth_vis = latest_output_->depth_vis.clone();
  out.depth_metric = latest_output_->depth_metric.clone();
  out.pose = latest_output_->pose;
  out.kf_a_idx = latest_output_->kf_a_idx;
  out.kf_b_idx = latest_output_->kf_b_idx;
  out.fx = latest_output_->fx;
  out.fy = latest_output_->fy;
  out.cx = latest_output_->cx;
  out.cy = latest_output_->cy;
  return out;
}

std::string Da3Worker::GetLatestStatus() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_status_;
}

void Da3Worker::ThreadMain() {
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [&]() { return stop_ || !jobs_.empty(); });
      if (stop_) {
        return;
      }
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }

    Da3Output out;
    const bool ok = runner_ && runner_->InferPair(job.a, job.b, &out);
    std::lock_guard<std::mutex> lock(mu_);
    if (!ok) {
      latest_output_.reset();
      if (!out.scale_text.empty()) {
        last_status_ = "DA3: " + out.scale_text;
      } else {
        last_status_ = cv::format("DA3: infer failed (kf%d,kf%d)", job.a.idx, job.b.idx);
      }
      continue;
    }

    double pair_chain_scale = 1.0;
    bool has_chain_scale = false;
    if (last_pair_cache_.has_value() &&
        last_pair_cache_->kf_b_idx == out.kf_a_idx &&
        !last_pair_cache_->depth_b_rel.empty() &&
        !out.depth_a_rel.empty()) {
      const std::optional<double> overlap_scale =
          MeanRatioScale(last_pair_cache_->depth_b_rel, out.depth_a_rel);
      if (overlap_scale.has_value() && std::isfinite(*overlap_scale)) {
        chained_world_scale_ *= *overlap_scale;
        pair_chain_scale = *overlap_scale;
        has_chain_scale = true;
      }
    } else if (!last_pair_cache_.has_value()) {
      chained_world_scale_ = 1.0;
    } else if (last_pair_cache_->kf_b_idx != out.kf_a_idx) {
      chained_world_scale_ = 1.0;
    }

    if (std::isfinite(chained_world_scale_) && chained_world_scale_ > 0.0) {
      out.depth_metric *= static_cast<float>(chained_world_scale_);
      out.depth_vis = ColorizeDepth(out.depth_metric);
      if (has_chain_scale) {
        out.scale_text = cv::format("scale_chain=%.4f total=%.4f", pair_chain_scale, chained_world_scale_);
      } else {
        out.scale_text = cv::format("scale_chain=1.0000 total=%.4f", chained_world_scale_);
      }
    }

    OverlapCache cache;
    cache.kf_b_idx = out.kf_b_idx;
    cache.depth_b_rel = out.depth_b_rel;
    latest_output_ = std::move(out);
    last_pair_cache_ = std::move(cache);
    last_status_ = cv::format("DA3: ok (kf%d,kf%d)", job.a.idx, job.b.idx);
  }
}

}  // namespace da3client
