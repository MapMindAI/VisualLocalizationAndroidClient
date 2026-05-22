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
constexpr char kFileMagic[] = "VLPREC1\n";
constexpr uint32_t kFileVersion = 1;
constexpr char kLogTag[] = "[mapmind_vlp_api]";
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

  void SetShuttingDown() {
    shutting_down_.store(true);
    cv_.notify_all();
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

  std::atomic<bool> recording_{false};
  std::unique_ptr<std::ofstream> record_ofs_;
  int64_t record_start_ts_ns_ = -1;
};

class ByteStreamService final : public grpc::Service {
 public:
  explicit ByteStreamService(StreamState* state) : state_(state) {
    AddMethod(new grpc::internal::RpcServiceMethod(
        kServiceMethod, grpc::internal::RpcMethod::SERVER_STREAMING,
        new grpc::internal::ServerStreamingHandler<ByteStreamService, grpc::ByteBuffer,
                                                   grpc::ByteBuffer>(
            std::mem_fn(&ByteStreamService::StreamFrames), this)));
  }

  grpc::Status StreamFrames(grpc::ServerContext* context, const grpc::ByteBuffer* request,
                            grpc::ServerWriter<grpc::ByteBuffer>* writer) {
    return state_->StreamFrames(context, request, writer);
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
  h[0] = 'V';
  h[1] = 'L';
  h[2] = 'P';
  h[3] = '2';
  std::memcpy(h + 8, &frame_timestamp_ns, sizeof(frame_timestamp_ns));
  std::memcpy(h + 16, &width, sizeof(width));
  std::memcpy(h + 20, &height, sizeof(height));
  std::memcpy(h + 24, &fx, sizeof(fx));
  std::memcpy(h + 28, &fy, sizeof(fy));
  std::memcpy(h + 32, &cx, sizeof(cx));
  std::memcpy(h + 36, &cy, sizeof(cy));
  std::memcpy(h + 40, &qx, sizeof(qx));
  std::memcpy(h + 44, &qy, sizeof(qy));
  std::memcpy(h + 48, &qz, sizeof(qz));
  std::memcpy(h + 52, &qw, sizeof(qw));
  std::memcpy(h + 56, &tx, sizeof(tx));
  std::memcpy(h + 60, &ty, sizeof(ty));
  (void)tz;
  std::memcpy(&payload[kHeaderSize], jpg_bytes.data(), jpg_bytes.size());
  PushFramePayload(frame_timestamp_ns, payload);
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

void SetLogsEnabled(bool enabled) { g_logs_enabled.store(enabled); }

}  // namespace vlp
}  // namespace mapmind
