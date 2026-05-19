#include "da3_mnn_runner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

#include "util.h"

#ifndef HELLO_AR_ENABLE_MNN_DA3
#define HELLO_AR_ENABLE_MNN_DA3 0
#endif

#if HELLO_AR_ENABLE_MNN_DA3
#include <MNN/Interpreter.hpp>
#include <MNN/Tensor.hpp>
#endif

namespace hello_ar {

namespace {
constexpr const char* kDa3MnnAssetPath = "models/da3_small_2_392x224_sim.mnn";
constexpr const char* kDa3MnnWeightAssetPath =
    "models/da3_small_2_392x224_sim.mnn.weight";
constexpr const char* kDa3CacheDir =
    "/data/user/0/com.google.ar.core.examples.c.helloar/cache";
constexpr const char* kDa3CacheModelPath =
    "/data/user/0/com.google.ar.core.examples.c.helloar/cache/da3_small_2_392x224_sim.mnn";
constexpr const char* kDa3CacheWeightPath =
    "/data/user/0/com.google.ar.core.examples.c.helloar/cache/da3_small_2_392x224_sim.mnn.weight";
}

#if HELLO_AR_ENABLE_MNN_DA3
struct Da3MnnImpl {
  std::shared_ptr<MNN::Interpreter> interpreter;
  MNN::Session* session = nullptr;
  MNN::Tensor* input = nullptr;
  int input_h = 224;
  int input_w = 392;
};
#endif

Da3MnnRunner::Da3MnnRunner(AAssetManager* asset_manager)
    : asset_manager_(asset_manager) {
  std::string error;
  initialized_ = Init(&error);
  init_error_ = error;
  if (!initialized_) {
    LOGI("Da3MnnRunner init failed: %s", init_error_.c_str());
  }
}

Da3MnnRunner::~Da3MnnRunner() {
#if HELLO_AR_ENABLE_MNN_DA3
  if (impl_ != nullptr) {
    auto* impl = reinterpret_cast<Da3MnnImpl*>(impl_);
    if (impl->interpreter && impl->session) {
      impl->interpreter->releaseSession(impl->session);
      impl->session = nullptr;
    }
    delete impl;
    impl_ = nullptr;
  }
#endif
}

bool Da3MnnRunner::IsReady() const { return initialized_; }

bool Da3MnnRunner::LoadModelFromAssets(std::vector<uint8_t>* model_bytes,
                                       std::string* error_msg) const {
  return LoadModelFromAssets(model_bytes, kDa3MnnAssetPath, error_msg);
}

bool Da3MnnRunner::LoadModelFromAssets(std::vector<uint8_t>* model_bytes,
                                       const char* asset_path,
                                       std::string* error_msg) const {
  if (asset_manager_ == nullptr) {
    if (error_msg) *error_msg = "asset_manager is null";
    return false;
  }
  AAsset* asset = AAssetManager_open(asset_manager_, asset_path, AASSET_MODE_BUFFER);
  if (asset == nullptr) {
    if (error_msg) *error_msg = std::string("cannot open asset: ") + asset_path;
    return false;
  }
  const size_t length = static_cast<size_t>(AAsset_getLength(asset));
  model_bytes->resize(length);
  const int read_size =
      AAsset_read(asset, model_bytes->data(), static_cast<int>(model_bytes->size()));
  AAsset_close(asset);
  if (read_size <= 0 || static_cast<size_t>(read_size) != model_bytes->size()) {
    if (error_msg) *error_msg = "failed to read complete model asset";
    return false;
  }
  return true;
}

bool Da3MnnRunner::WriteFile(const char* path, const std::vector<uint8_t>& bytes,
                             std::string* error_msg) const {
  FILE* fp = std::fopen(path, "wb");
  if (fp == nullptr) {
    if (error_msg) *error_msg = std::string("failed to open file for write: ") + path;
    return false;
  }
  const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), fp);
  std::fclose(fp);
  if (written != bytes.size()) {
    if (error_msg) *error_msg = std::string("failed to write full file: ") + path;
    return false;
  }
  return true;
}

bool Da3MnnRunner::Init(std::string* error_msg) {
#if !HELLO_AR_ENABLE_MNN_DA3
  if (error_msg) *error_msg = "MNN disabled at compile time";
  return false;
#else
  std::vector<uint8_t> model_bytes;
  if (!LoadModelFromAssets(&model_bytes, kDa3MnnAssetPath, error_msg)) {
    return false;
  }
  std::vector<uint8_t> weight_bytes;
  const bool has_weight =
      LoadModelFromAssets(&weight_bytes, kDa3MnnWeightAssetPath, nullptr);

  // For MNN models with external weight files, load from real files in cache.
  if (::mkdir(kDa3CacheDir, 0755) != 0) {
    // ignore EEXIST
  }
  if (!WriteFile(kDa3CacheModelPath, model_bytes, error_msg)) {
    return false;
  }
  if (has_weight) {
    if (!WriteFile(kDa3CacheWeightPath, weight_bytes, error_msg)) {
      return false;
    }
  }

  auto* impl = new Da3MnnImpl();
  impl->interpreter = std::shared_ptr<MNN::Interpreter>(
      MNN::Interpreter::createFromFile(kDa3CacheModelPath));
  if (!impl->interpreter) {
    if (error_msg) *error_msg = "MNN::Interpreter::createFromFile failed";
    delete impl;
    return false;
  }

  MNN::ScheduleConfig config;
  config.type = MNN_FORWARD_CPU;
  config.numThread = 4;
  MNN::BackendConfig backend_config;
  backend_config.precision = MNN::BackendConfig::Precision_Normal;
  config.backendConfig = &backend_config;
  impl->session = impl->interpreter->createSession(config);
  if (impl->session == nullptr) {
    if (error_msg) *error_msg = "createSession failed";
    delete impl;
    return false;
  }

  impl->input = impl->interpreter->getSessionInput(impl->session, nullptr);
  if (impl->input == nullptr) {
    if (error_msg) *error_msg = "getSessionInput failed";
    delete impl;
    return false;
  }

  const auto shape = impl->input->shape();
  if (shape.size() == 5) {
    // Expected shape is [1,2,3,224,392].
    impl->input_h = shape[3];
    impl->input_w = shape[4];
  }
  impl_ = impl;
  return true;
#endif
}

bool Da3MnnRunner::RunPair(const std::vector<uint8_t>& prev_gray,
                           const std::vector<uint8_t>& curr_gray, int width,
                           int height, std::string* result_msg) {
#if !HELLO_AR_ENABLE_MNN_DA3
  if (result_msg) *result_msg = "MNN disabled";
  return false;
#else
  if (!initialized_ || impl_ == nullptr) {
    if (result_msg) *result_msg = init_error_;
    return false;
  }
  if (width <= 0 || height <= 0) {
    if (result_msg) *result_msg = "invalid input image size";
    return false;
  }
  if (prev_gray.size() != curr_gray.size() || prev_gray.size() !=
      static_cast<size_t>(width) * static_cast<size_t>(height)) {
    if (result_msg) *result_msg = "gray size mismatch";
    return false;
  }

  auto* impl = reinterpret_cast<Da3MnnImpl*>(impl_);
  const int out_h = std::max(1, impl->input_h);
  const int out_w = std::max(1, impl->input_w);

  auto resize_nearest = [&](const std::vector<uint8_t>& src) {
    std::vector<uint8_t> dst(static_cast<size_t>(out_h) * static_cast<size_t>(out_w));
    for (int y = 0; y < out_h; ++y) {
      const int src_y = std::min(height - 1, y * height / out_h);
      for (int x = 0; x < out_w; ++x) {
        const int src_x = std::min(width - 1, x * width / out_w);
        dst[static_cast<size_t>(y) * out_w + x] =
            src[static_cast<size_t>(src_y) * width + src_x];
      }
    }
    return dst;
  };

  const std::vector<uint8_t> prev_resized = resize_nearest(prev_gray);
  const std::vector<uint8_t> curr_resized = resize_nearest(curr_gray);
  std::vector<float> input_data(static_cast<size_t>(2) * 3 * out_h * out_w);

  auto fill_frame = [&](const std::vector<uint8_t>& gray, int frame_idx) {
    for (int c = 0; c < 3; ++c) {
      for (int y = 0; y < out_h; ++y) {
        for (int x = 0; x < out_w; ++x) {
          const size_t src_idx = static_cast<size_t>(y) * out_w + x;
          const size_t dst_idx =
              ((static_cast<size_t>(frame_idx) * 3 + c) * out_h + y) * out_w + x;
          input_data[dst_idx] = static_cast<float>(gray[src_idx]) / 255.0f;
        }
      }
    }
  };
  fill_frame(prev_resized, 0);
  fill_frame(curr_resized, 1);

  std::shared_ptr<MNN::Tensor> host_input(
      MNN::Tensor::create<float>({1, 2, 3, out_h, out_w}, input_data.data(),
                                 MNN::Tensor::CAFFE));
  if (!host_input) {
    if (result_msg) *result_msg = "MNN host input allocation failed";
    return false;
  }
  impl->input->copyFromHostTensor(host_input.get());

  const auto t0 = std::chrono::steady_clock::now();
  impl->interpreter->runSession(impl->session);
  const auto t1 = std::chrono::steady_clock::now();
  const double infer_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  auto outputs = impl->interpreter->getSessionOutputAll(impl->session);
  std::ostringstream oss;
  oss << "infer=" << infer_ms << "ms outputs=" << outputs.size() << " ";
  for (const auto& kv : outputs) {
    auto* t = kv.second;
    if (t == nullptr) continue;
    const auto shape = t->shape();
    oss << kv.first << ":(";
    for (size_t i = 0; i < shape.size(); ++i) {
      oss << shape[i];
      if (i + 1 < shape.size()) oss << ",";
    }
    oss << ") ";
  }
  if (result_msg) *result_msg = oss.str();
  return true;
#endif
}

}  // namespace hello_ar
