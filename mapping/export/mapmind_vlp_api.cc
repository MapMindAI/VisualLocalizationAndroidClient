// Copyright 2025 DeepMirror Inc. All rights reserved.

#include "mapping/export/mapmind_vlp_api.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/codegen/method_handler.h>
#include <grpcpp/impl/service_type.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace mapmind {
namespace vlp {
namespace {

constexpr char kServiceMethod[] = "/vlp.FrameStreamService/StreamFrames";
constexpr char kControlMethod[] = "/vlp.ControlService/SendControl";
constexpr char kFileMagic[] = "VLPREC1\n";
constexpr uint32_t kFileVersion = 1;
constexpr char kLogTag[] = "[mapmind_vlp_api]";
constexpr uint32_t kFrameMagic = 0x564C5032;
constexpr char kDepthTag[] = "DPT1";
std::atomic<bool> g_logs_enabled{true};

void Logf(const char* level, const char* fmt, ...) {
  if (!g_logs_enabled.load()) return;
  std::printf("%s %s ", kLogTag, level);
  va_list args;
  va_start(args, fmt);
  std::vprintf(fmt, args);
  va_end(args);
  std::printf("\n");
}

void WriteU32LE(std::ostream* os, uint32_t v) {
  char b[4];
  b[0] = static_cast<char>(v & 0xFFu);
  b[1] = static_cast<char>((v >> 8) & 0xFFu);
  b[2] = static_cast<char>((v >> 16) & 0xFFu);
  b[3] = static_cast<char>((v >> 24) & 0xFFu);
  os->write(b, 4);
}

void WriteU64LE(std::ostream* os, uint64_t v) {
  char b[8];
  b[0] = static_cast<char>(v & 0xFFu);
  b[1] = static_cast<char>((v >> 8) & 0xFFu);
  b[2] = static_cast<char>((v >> 16) & 0xFFu);
  b[3] = static_cast<char>((v >> 24) & 0xFFu);
  b[4] = static_cast<char>((v >> 32) & 0xFFu);
  b[5] = static_cast<char>((v >> 40) & 0xFFu);
  b[6] = static_cast<char>((v >> 48) & 0xFFu);
  b[7] = static_cast<char>((v >> 56) & 0xFFu);
  os->write(b, 8);
}

void AppendU32LE(std::string* out, uint32_t v) {
  char b[4];
  b[0] = static_cast<char>(v & 0xFFu);
  b[1] = static_cast<char>((v >> 8) & 0xFFu);
  b[2] = static_cast<char>((v >> 16) & 0xFFu);
  b[3] = static_cast<char>((v >> 24) & 0xFFu);
  out->append(b, 4);
}

void AppendU64LE(std::string* out, uint64_t v) {
  char b[8];
  b[0] = static_cast<char>(v & 0xFFu);
  b[1] = static_cast<char>((v >> 8) & 0xFFu);
  b[2] = static_cast<char>((v >> 16) & 0xFFu);
  b[3] = static_cast<char>((v >> 24) & 0xFFu);
  b[4] = static_cast<char>((v >> 32) & 0xFFu);
  b[5] = static_cast<char>((v >> 40) & 0xFFu);
  b[6] = static_cast<char>((v >> 48) & 0xFFu);
  b[7] = static_cast<char>((v >> 56) & 0xFFu);
  out->append(b, 8);
}

class StreamState {
 public:
  void PublishFrame(int64_t frame_timestamp_ns, const std::string& payload) {
    if (payload.empty()) {
      Logf("I", "Drop empty frame payload");
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    latest_payload_ = payload;
    latest_frame_ts_ns_ = frame_timestamp_ns;
    ++frame_seq_;
    MaybeRecordLocked(frame_timestamp_ns, payload);
    cv_.notify_all();
  }

  grpc::Status StreamFrames(grpc::ServerContext* context, const grpc::ByteBuffer*,
                            grpc::ServerWriter<grpc::ByteBuffer>* writer) {
    Logf("I", "Frame stream connected");
    uint64_t seen_seq = 0;
    for (;;) {
      std::string payload;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() {
          return shutting_down_.load() || context->IsCancelled() || frame_seq_ > seen_seq;
        });
        if (shutting_down_.load() || context->IsCancelled()) {
          Logf("I", "Frame stream disconnected");
          return grpc::Status::OK;
        }
        seen_seq = frame_seq_;
        payload = latest_payload_;
      }

      grpc::Slice slice(payload.data(), payload.size());
      grpc::ByteBuffer out(&slice, 1);
      if (!writer->Write(out)) {
        Logf("I", "Frame stream closed by peer");
        return grpc::Status::OK;
      }
    }
  }

  bool StartRecording(const std::string& output_file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    StopRecordingLocked();
    record_ofs_ = std::make_unique<std::ofstream>(output_file_path,
                                                  std::ios::binary | std::ios::trunc);
    if (!record_ofs_ || !record_ofs_->good()) {
      Logf("E", "Failed to open recording file: %s", output_file_path.c_str());
      record_ofs_.reset();
      return false;
    }
    record_ofs_->write(kFileMagic, 8);
    WriteU32LE(record_ofs_.get(), kFileVersion);
    record_ofs_->flush();
    if (!record_ofs_->good()) {
      Logf("E", "Failed to initialize recording file: %s", output_file_path.c_str());
      StopRecordingLocked();
      return false;
    }
    recording_.store(true);
    record_start_ts_ns_ = -1;
    Logf("I", "Recording started: %s", output_file_path.c_str());
    return true;
  }

  void StopRecording() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_.load()) {
      Logf("I", "Recording stopped");
    }
    StopRecordingLocked();
  }

  bool RecordingEnabled() const { return recording_.load(); }

  void SetControlCommandCallback(ControlCommandCallback callback, void* user_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    control_callback_ = callback;
    control_user_data_ = user_data;
  }

  void PublishControlCommand(int cmd) {
    ControlCommandCallback callback = nullptr;
    void* user_data = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = control_callback_;
      user_data = control_user_data_;
    }
    if (callback != nullptr) {
      callback(cmd, user_data);
    }
  }

  void SetShuttingDown() {
    shutting_down_.store(true);
    cv_.notify_all();
  }

  void UpdateDepth(int64_t rgb_timestamp_ns, int width, int height,
                   const uint8_t* depth_bytes, size_t depth_size) {
    if (depth_bytes == nullptr || depth_size == 0 || width <= 0 || height <= 0 ||
        rgb_timestamp_ns <= 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pending_depth_.rgb_timestamp_ns = rgb_timestamp_ns;
    pending_depth_.width = width;
    pending_depth_.height = height;
    pending_depth_.bytes.assign(depth_bytes, depth_bytes + depth_size);
    pending_depth_.depth_seq += 1;
    pending_depth_.published = false;
  }

  bool ConsumeDepthForFrame(int64_t frame_timestamp_ns, int64_t* rgb_timestamp_ns, int* width,
                            int* height, std::vector<uint8_t>* bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_depth_.bytes.empty() || pending_depth_.published) return false;
    if (frame_timestamp_ns < pending_depth_.rgb_timestamp_ns) return false;
    if (rgb_timestamp_ns) *rgb_timestamp_ns = pending_depth_.rgb_timestamp_ns;
    if (width) *width = pending_depth_.width;
    if (height) *height = pending_depth_.height;
    if (bytes) *bytes = pending_depth_.bytes;
    pending_depth_.published = true;  // each depth publish once
    return true;
  }

 private:
  void StopRecordingLocked() {
    recording_.store(false);
    record_start_ts_ns_ = -1;
    if (record_ofs_) {
      record_ofs_->flush();
      record_ofs_->close();
      record_ofs_.reset();
    }
  }

  void MaybeRecordLocked(int64_t frame_timestamp_ns, const std::string& payload) {
    if (!recording_.load() || !record_ofs_) return;
    if (record_start_ts_ns_ < 0) record_start_ts_ns_ = frame_timestamp_ns;
    const uint64_t rel_ns = static_cast<uint64_t>(
        frame_timestamp_ns > record_start_ts_ns_ ? frame_timestamp_ns - record_start_ts_ns_ : 0);
    WriteU64LE(record_ofs_.get(), rel_ns);
    WriteU32LE(record_ofs_.get(), static_cast<uint32_t>(payload.size()));
    record_ofs_->write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (!record_ofs_->good()) StopRecordingLocked();
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::string latest_payload_;
  int64_t latest_frame_ts_ns_ = 0;
  uint64_t frame_seq_ = 0;
  std::atomic<bool> shutting_down_{false};
  ControlCommandCallback control_callback_ = nullptr;
  void* control_user_data_ = nullptr;

  std::atomic<bool> recording_{false};
  std::unique_ptr<std::ofstream> record_ofs_;
  int64_t record_start_ts_ns_ = -1;

  struct PendingDepth {
    int64_t rgb_timestamp_ns = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bytes;
    uint64_t depth_seq = 0;
    bool published = true;
  } pending_depth_;
};

class ByteStreamService final : public grpc::Service {
 public:
  explicit ByteStreamService(StreamState* state) : state_(state) {
    AddMethod(new grpc::internal::RpcServiceMethod(
        kServiceMethod, grpc::internal::RpcMethod::SERVER_STREAMING,
        new grpc::internal::ServerStreamingHandler<ByteStreamService, grpc::ByteBuffer,
                                                   grpc::ByteBuffer>(
            std::mem_fn(&ByteStreamService::StreamFrames), this)));
    AddMethod(new grpc::internal::RpcServiceMethod(
        kControlMethod, grpc::internal::RpcMethod::NORMAL_RPC,
        new grpc::internal::RpcMethodHandler<ByteStreamService, grpc::ByteBuffer,
                                             grpc::ByteBuffer>(
            std::mem_fn(&ByteStreamService::SendControl), this)));
  }

  grpc::Status StreamFrames(grpc::ServerContext* context, const grpc::ByteBuffer* request,
                            grpc::ServerWriter<grpc::ByteBuffer>* writer) {
    return state_->StreamFrames(context, request, writer);
  }

  grpc::Status SendControl(grpc::ServerContext*, const grpc::ByteBuffer* request,
                           grpc::ByteBuffer* response) {
    if (request == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "empty request");
    }
    std::vector<grpc::Slice> slices;
    const grpc::Status dump_status = request->Dump(&slices);
    if (!dump_status.ok()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid request");
    }
    std::string payload;
    for (const auto& slice : slices) {
      payload.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
    }
    if (payload.size() < 4) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "control payload < 4 bytes");
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.data());
    const int cmd = static_cast<int>(p[0]) | (static_cast<int>(p[1]) << 8) |
                    (static_cast<int>(p[2]) << 16) | (static_cast<int>(p[3]) << 24);
    state_->PublishControlCommand(cmd);

    const std::string ack = "ok:" + std::to_string(cmd);
    grpc::Slice out_slice(ack.data(), ack.size());
    *response = grpc::ByteBuffer(&out_slice, 1);
    return grpc::Status::OK;
  }

 private:
  StreamState* state_ = nullptr;
};

struct GlobalState {
  std::mutex mutex;
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<ByteStreamService> service;
  std::unique_ptr<StreamState> stream_state;
  std::atomic<bool> running{false};
};

GlobalState& G() {
  static GlobalState s;
  return s;
}

}  // namespace

bool StartGrpcServer(int port) {
  auto& g = G();
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.running.load()) {
    Logf("I", "gRPC server already running");
    return true;
  }

  g.stream_state = std::make_unique<StreamState>();
  g.service = std::make_unique<ByteStreamService>(g.stream_state.get());

  grpc::ServerBuilder builder;
  builder.AddListeningPort("0.0.0.0:" + std::to_string(port),
                           grpc::InsecureServerCredentials());
  builder.RegisterService(g.service.get());
  g.server = builder.BuildAndStart();
  if (!g.server) {
    Logf("E", "Failed to start gRPC server on port %d", port);
    g.service.reset();
    g.stream_state.reset();
    return false;
  }
  g.running.store(true);
  Logf("I", "gRPC server started on port %d", port);
  return true;
}

void StopGrpcServer() {
  auto& g = G();
  std::lock_guard<std::mutex> lock(g.mutex);
  if (!g.running.load()) {
    Logf("I", "gRPC server already stopped");
    return;
  }
  Logf("I", "Stopping gRPC server");
  if (g.stream_state) g.stream_state->SetShuttingDown();
  if (g.server) g.server->Shutdown();
  if (g.server) g.server->Wait();
  g.server.reset();
  g.service.reset();
  g.stream_state.reset();
  g.running.store(false);
  Logf("I", "gRPC server stopped");
}

bool HasGrpcServer() { return G().running.load(); }

void PushFramePayload(int64_t frame_timestamp_ns, const std::string& payload) {
  auto& g = G();
  if (!g.running.load() || !g.stream_state) return;
  g.stream_state->PublishFrame(frame_timestamp_ns, payload);
}

void PushFrameYuvNv21(int64_t frame_timestamp_ns, int width, int height,
                      const uint8_t* yuv_nv21, size_t yuv_nv21_size, float fx, float fy,
                      float cx, float cy, float qx, float qy, float qz, float qw,
                      float tx, float ty, float tz) {
  if (width <= 0 || height <= 0 || yuv_nv21 == nullptr || yuv_nv21_size == 0) return;
  const size_t expected_nv21_size =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
  if (yuv_nv21_size < expected_nv21_size) {
    Logf("E", "Invalid NV21 buffer size: got=%zu expected>=%zu", yuv_nv21_size,
         expected_nv21_size);
    return;
  }

  cv::Mat nv21(height + height / 2, width, CV_8UC1, const_cast<uint8_t*>(yuv_nv21));
  cv::Mat bgr;
  cv::cvtColor(nv21, bgr, cv::COLOR_YUV2BGR_NV21);
  std::vector<uint8_t> jpg_bytes;
  const std::vector<int> encode_params = {cv::IMWRITE_JPEG_QUALITY, 80};
  if (!cv::imencode(".jpg", bgr, jpg_bytes, encode_params) || jpg_bytes.empty()) {
    Logf("E", "JPEG encode failed for frame ts=%lld",
         static_cast<long long>(frame_timestamp_ns));
    return;
  }

  constexpr size_t kHeaderSize = 64;
  std::string payload(kHeaderSize + jpg_bytes.size(), '\0');
  char* h = &payload[0];
  std::memcpy(h + 0, &kFrameMagic, sizeof(kFrameMagic));
  std::memcpy(h + 4, &frame_timestamp_ns, sizeof(frame_timestamp_ns));
  std::memcpy(h + 12, &width, sizeof(width));
  std::memcpy(h + 16, &height, sizeof(height));
  std::memcpy(h + 20, &fx, sizeof(fx));
  std::memcpy(h + 24, &fy, sizeof(fy));
  std::memcpy(h + 28, &cx, sizeof(cx));
  std::memcpy(h + 32, &cy, sizeof(cy));
  std::memcpy(h + 36, &qx, sizeof(qx));
  std::memcpy(h + 40, &qy, sizeof(qy));
  std::memcpy(h + 44, &qz, sizeof(qz));
  std::memcpy(h + 48, &qw, sizeof(qw));
  std::memcpy(h + 52, &tx, sizeof(tx));
  std::memcpy(h + 56, &ty, sizeof(ty));
  std::memcpy(h + 60, &tz, sizeof(tz));

  // Attach latest depth once when available.
  auto& g = G();
  if (g.stream_state) {
    int64_t depth_rgb_ts = 0;
    int depth_w = 0;
    int depth_h = 0;
    std::vector<uint8_t> depth_bytes;
    if (g.stream_state->ConsumeDepthForFrame(frame_timestamp_ns, &depth_rgb_ts, &depth_w,
                                             &depth_h, &depth_bytes)) {
      payload.append(kDepthTag, 4);
      AppendU64LE(&payload, static_cast<uint64_t>(depth_rgb_ts));
      AppendU32LE(&payload, static_cast<uint32_t>(depth_w));
      AppendU32LE(&payload, static_cast<uint32_t>(depth_h));
      AppendU32LE(&payload, static_cast<uint32_t>(depth_bytes.size()));
      payload.append(reinterpret_cast<const char*>(depth_bytes.data()), depth_bytes.size());
    }
  }
  std::memcpy(&payload[kHeaderSize], jpg_bytes.data(), jpg_bytes.size());
  PushFramePayload(frame_timestamp_ns, payload);
}

void PushDepthUpdate(int64_t rgb_timestamp_ns, int width, int height, const uint8_t* depth_bytes,
                     size_t depth_size) {
  auto& g = G();
  if (!g.stream_state) return;
  g.stream_state->UpdateDepth(rgb_timestamp_ns, width, height, depth_bytes, depth_size);
}

bool StartRecording(const std::string& output_file_path) {
  auto& g = G();
  if (!g.stream_state) return false;
  return g.stream_state->StartRecording(output_file_path);
}

bool Recording() {
  auto& g = G();
  if (!g.stream_state) return false;
  return g.stream_state->RecordingEnabled();
}

void StopRecording() {
  auto& g = G();
  if (g.stream_state) g.stream_state->StopRecording();
}

void SetControlCommandCallback(ControlCommandCallback callback, void* user_data) {
  auto& g = G();
  if (!g.stream_state) return;
  g.stream_state->SetControlCommandCallback(callback, user_data);
}

void SetLogsEnabled(bool enabled) { g_logs_enabled.store(enabled); }

}  // namespace vlp
}  // namespace mapmind
