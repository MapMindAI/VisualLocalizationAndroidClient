/*
 * Copyright 2026
 */

#include "da2_pipeline.h"

#include <android/asset_manager.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>

#include "MNN/Interpreter.hpp"
#include "util.h"

namespace hello_ar {
namespace {

}  // namespace

struct Da2Pipeline::Impl {
  explicit Impl(AAssetManager* asset_manager) : asset_manager(asset_manager) {
    init_thread = std::thread(&Impl::InitializeLoop, this);
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
      has_pending = false;
    }
    cv.notify_all();
    if (init_thread.joinable()) {
      init_thread.join();
    }
    if (worker.joinable()) {
      worker.join();
    }
  }

  struct Frame {
    std::vector<uint8_t> gray;
    int width = 0;
    int height = 0;
    int64_t timestamp_ns = 0;
  };

  void InitializeLoop() {
    const bool ok = Initialize();
    if (!ok) {
      LOGE("DA2 pipeline initialization failed.");
      return;
    }
    ready.store(true);
    worker = std::thread(&Impl::WorkerLoop, this);
    LOGI("DA2 pipeline initialized.");
  }

  bool Initialize() {
    std::vector<uint8_t> model_data;
    if (!ReadAssetFile("models/depth_anything_v2_metric_vits.mnn", &model_data)) {
      LOGE("DA2 model asset read failed.");
      return false;
    }

    interpreter.reset(
        MNN::Interpreter::createFromBuffer(model_data.data(), model_data.size()),
        MNN::Interpreter::destroy);
    if (!interpreter) {
      LOGE("Create MNN interpreter failed.");
      return false;
    }

    MNN::ScheduleConfig config;
    MNN::BackendConfig backend_config;
    backend_config.precision = MNN::BackendConfig::Precision_Low;
    backend_config.power = MNN::BackendConfig::Power_High;
    backend_config.memory = MNN::BackendConfig::Memory_Normal;
    config.backendConfig = &backend_config;
    config.type = MNN_FORWARD_VULKAN;
    config.numThread = 2;
    session = interpreter->createSession(config);
    if (session == nullptr) {
      LOGI("Create MNN Vulkan session failed, fallback to OpenCL.");
      config.type = MNN_FORWARD_OPENCL;
      session = interpreter->createSession(config);
    }
    if (session == nullptr) {
      LOGI("Create MNN OpenCL session failed, fallback to CPU.");
      config.type = MNN_FORWARD_CPU;
      config.numThread = 4;
      session = interpreter->createSession(config);
    }
    if (session == nullptr) {
      LOGE("Create MNN session failed.");
      return false;
    }
    const char* backend_name = "CPU";
    if (config.type == MNN_FORWARD_VULKAN) {
      backend_name = "Vulkan";
    } else if (config.type == MNN_FORWARD_OPENCL) {
      backend_name = "OpenCL";
    }
    LOGI("DA2 session backend: %s", backend_name);

    input_tensor = interpreter->getSessionInput(session, nullptr);
    if (input_tensor == nullptr) {
      LOGE("Get DA2 input tensor failed.");
      return false;
    }

    std::vector<int> input_shape = input_tensor->shape();
    if (input_shape.size() != 4 || input_shape[0] != 1 || input_shape[1] != 3 ||
        input_shape[2] <= 0 || input_shape[3] <= 0) {
      LOGE("Unexpected DA2 input shape.");
      return false;
    }

    input_height = input_shape[2];
    input_width = input_shape[3];
    input_host.reset(new MNN::Tensor(input_tensor, MNN::Tensor::CAFFE));
    if (!input_host || input_host->host<float>() == nullptr) {
      LOGE("Create DA2 host input tensor failed.");
      return false;
    }

    LOGI("DA2 input shape: 1x3x%dx%d", input_height, input_width);
    return true;
  }

  bool ReadAssetFile(const char* path, std::vector<uint8_t>* out) {
    if (asset_manager == nullptr || out == nullptr) {
      return false;
    }
    AAsset* asset = AAssetManager_open(asset_manager, path, AASSET_MODE_BUFFER);
    if (asset == nullptr) {
      return false;
    }
    const off_t len = AAsset_getLength(asset);
    if (len <= 0) {
      AAsset_close(asset);
      return false;
    }
    out->resize(static_cast<size_t>(len));
    const int read_len = AAsset_read(asset, out->data(), len);
    AAsset_close(asset);
    return read_len == len;
  }

  void WorkerLoop() {
    for (;;) {
      Frame task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]() { return stop || has_pending; });
        if (stop) {
          return;
        }
        task = std::move(pending);
        has_pending = false;
      }
      ProcessTask(task);
    }
  }

  void ProcessTask(const Frame& task) {
    const int expected_size = task.width * task.height;
    if (static_cast<int>(task.gray.size()) < expected_size) {
      return;
    }

    const auto prep_t0 = std::chrono::steady_clock::now();
    float* input_ptr = input_host->host<float>();
    const int pixel_count = input_width * input_height;
    const uint8_t* y_plane = task.gray.data();
    for (int dy = 0; dy < input_height; ++dy) {
      const int sy = std::min(task.height - 1, (dy * task.height) / input_height);
      for (int dx = 0; dx < input_width; ++dx) {
        const int sx = std::min(task.width - 1, (dx * task.width) / input_width);
        const float yv =
            static_cast<float>(y_plane[sy * task.width + sx]) / 255.0f;

        const int i = dy * input_width + dx;
        input_ptr[i] = yv;
        input_ptr[pixel_count + i] = yv;
        input_ptr[2 * pixel_count + i] = yv;
      }
    }

    const auto prep_t1 = std::chrono::steady_clock::now();

    input_tensor->copyFromHostTensor(input_host.get());
    const auto infer_t0 = std::chrono::steady_clock::now();
    const MNN::ErrorCode rc = interpreter->runSession(session);
    if (rc != MNN::NO_ERROR) {
      LOGE("DA2 runSession failed: %d", static_cast<int>(rc));
      return;
    }
    const auto infer_t1 = std::chrono::steady_clock::now();
    const double prep_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                               prep_t1 - prep_t0)
                               .count() /
                           1000.0;
    const double infer_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                                infer_t1 - infer_t0)
                                .count() /
                            1000.0;

    MNN::Tensor* output_tensor = interpreter->getSessionOutput(session, nullptr);
    if (output_tensor != nullptr) {
      MNN::Tensor output_host(output_tensor, MNN::Tensor::CAFFE);
      output_tensor->copyToHostTensor(&output_host);
      const float* depth_ptr = output_host.host<float>();
      const std::vector<int> out_shape = output_host.shape();
      if (depth_ptr != nullptr) {
        int out_h = 0;
        int out_w = 0;
        if (out_shape.size() == 4) {
          if (out_shape[1] == 1) {
            out_h = out_shape[2];
            out_w = out_shape[3];
          } else if (out_shape[3] == 1) {
            out_h = out_shape[1];
            out_w = out_shape[2];
          } else {
            out_h = out_shape[2];
            out_w = out_shape[3];
          }
        } else if (out_shape.size() == 3) {
          if (out_shape[0] == 1) {
            out_h = out_shape[1];
            out_w = out_shape[2];
          } else if (out_shape[2] == 1) {
            out_h = out_shape[0];
            out_w = out_shape[1];
          } else {
            out_h = out_shape[1];
            out_w = out_shape[2];
          }
        } else if (out_shape.size() == 2) {
          out_h = out_shape[0];
          out_w = out_shape[1];
        }

        if (out_h > 0 && out_w > 0) {
          const int out_count = out_h * out_w;
          std::vector<uint8_t> preview_depth_rg(static_cast<size_t>(out_count) * 2U);
          for (int i = 0; i < out_count; ++i) {
            const uint32_t depth_mm = static_cast<uint32_t>(depth_ptr[i] * 1000.0f);
            preview_depth_rg[2 * i + 0] = static_cast<uint8_t>(depth_mm & 0xFFu);
            preview_depth_rg[2 * i + 1] = static_cast<uint8_t>((depth_mm >> 8) & 0xFFu);
          }

          {
            std::lock_guard<std::mutex> lock(preview_mutex);
            latest_depth_preview.depth_rg = std::move(preview_depth_rg);
            latest_depth_preview.width = out_w;
            latest_depth_preview.height = out_h;
            latest_depth_preview.timestamp_ns = task.timestamp_ns;
          }
        }
      }
    }

    ++processed_frames;
    if ((processed_frames % 1) == 0) {
      LOGI("DA2 processed frame ts=%lld prepare=%.2f ms inference=%.2f ms",
           static_cast<long long>(task.timestamp_ns), prep_ms, infer_ms);
    }
  }

  AAssetManager* asset_manager = nullptr;
  std::atomic<bool> ready{false};
  std::shared_ptr<MNN::Interpreter> interpreter;
  MNN::Session* session = nullptr;
  MNN::Tensor* input_tensor = nullptr;
  std::unique_ptr<MNN::Tensor> input_host;
  int input_width = 0;
  int input_height = 0;

  std::thread init_thread;
  std::thread worker;
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false;
  bool has_pending = false;
  Frame pending;
  int64_t processed_frames = 0;

  mutable std::mutex preview_mutex;
  Da2Pipeline::DepthPreview latest_depth_preview;
};

Da2Pipeline::Da2Pipeline(AAssetManager* asset_manager)
    : impl_(new Impl(asset_manager)) {}

Da2Pipeline::~Da2Pipeline() = default;

void Da2Pipeline::EnqueueFrame(const std::vector<uint8_t>& gray, int width,
                               int height, int64_t timestamp_ns) {
  if (!impl_ || !impl_->ready.load() || gray.empty() || width <= 0 ||
      height <= 0) {
    return;
  }
  Impl::Frame task;
  task.gray = gray;
  task.width = width;
  task.height = height;
  task.timestamp_ns = timestamp_ns;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pending = std::move(task);
    impl_->has_pending = true;
  }
  impl_->cv.notify_one();
}

bool Da2Pipeline::GetLatestDepthPreview(DepthPreview* out) const {
  if (!impl_ || !impl_->ready.load() || out == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->preview_mutex);
  if (impl_->latest_depth_preview.depth_rg.empty() ||
      impl_->latest_depth_preview.width <= 0 ||
      impl_->latest_depth_preview.height <= 0 ||
      impl_->latest_depth_preview.timestamp_ns <= 0) {
    return false;
  }
  *out = impl_->latest_depth_preview;
  return true;
}

bool Da2Pipeline::IsReady() const {
  return impl_ != nullptr && impl_->ready.load();
}

}  // namespace hello_ar
