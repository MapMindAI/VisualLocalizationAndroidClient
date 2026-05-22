// Copyright 2025 DeepMirror Inc. All rights reserved.

#include "mapping/export/mapmind_vlp_api.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/codegen/method_handler.h>
#include <grpcpp/impl/service_type.h>
#include <grpc/support/log.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

namespace mapmind::vlp {
namespace {

constexpr char kServiceMethod[] = "/vlp.FrameStreamService/StreamFrames";
constexpr char kFileMagic[] = "VLPREC1\n";
constexpr uint32_t kFileVersion = 1;
constexpr char kLogTag[] = "[mapmind_vlp_api]";

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
      gpr_log(GPR_INFO, "%s Drop empty frame payload", kLogTag);
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
    gpr_log(GPR_INFO, "%s Frame stream connected", kLogTag);
    uint64_t seen_seq = 0;
    for (;;) {
      std::string payload;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() {
          return shutting_down_.load() || context->IsCancelled() || frame_seq_ > seen_seq;
        });
        if (shutting_down_.load() || context->IsCancelled()) {
          gpr_log(GPR_INFO, "%s Frame stream disconnected", kLogTag);
          return grpc::Status::OK;
        }
        seen_seq = frame_seq_;
        payload = latest_payload_;
      }

      grpc::Slice slice(payload.data(), payload.size());
      grpc::ByteBuffer out(&slice, 1);
      if (!writer->Write(out)) {
        gpr_log(GPR_INFO, "%s Frame stream closed by peer", kLogTag);
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
      gpr_log(GPR_ERROR, "%s Failed to open recording file: %s", kLogTag,
              output_file_path.c_str());
      record_ofs_.reset();
      return false;
    }
    record_ofs_->write(kFileMagic, 8);
    WriteU32LE(record_ofs_.get(), kFileVersion);
    record_ofs_->flush();
    if (!record_ofs_->good()) {
      gpr_log(GPR_ERROR, "%s Failed to initialize recording file: %s", kLogTag,
              output_file_path.c_str());
      StopRecordingLocked();
      return false;
    }
    recording_.store(true);
    record_start_ts_ns_ = -1;
    gpr_log(GPR_INFO, "%s Recording started: %s", kLogTag, output_file_path.c_str());
    return true;
  }

  void StopRecording() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_.load()) {
      gpr_log(GPR_INFO, "%s Recording stopped", kLogTag);
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
    gpr_log(GPR_INFO, "%s gRPC server already running", kLogTag);
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
    gpr_log(GPR_ERROR, "%s Failed to start gRPC server on port %d", kLogTag, port);
    g.service.reset();
    g.stream_state.reset();
    return false;
  }
  g.running.store(true);
  gpr_log(GPR_INFO, "%s gRPC server started on port %d", kLogTag, port);
  return true;
}

void StopGrpcServer() {
  auto& g = G();
  std::lock_guard<std::mutex> lock(g.mutex);
  if (!g.running.load()) {
    gpr_log(GPR_INFO, "%s gRPC server already stopped", kLogTag);
    return;
  }
  gpr_log(GPR_INFO, "%s Stopping gRPC server", kLogTag);
  if (g.stream_state) g.stream_state->SetShuttingDown();
  if (g.server) g.server->Shutdown();
  if (g.server) g.server->Wait();
  g.server.reset();
  g.service.reset();
  g.stream_state.reset();
  g.running.store(false);
  gpr_log(GPR_INFO, "%s gRPC server stopped", kLogTag);
}

bool HasGrpcServer() { return G().running.load(); }

void PushFramePayload(int64_t frame_timestamp_ns, const std::string& payload) {
  auto& g = G();
  if (!g.running.load() || !g.stream_state) return;
  g.stream_state->PublishFrame(frame_timestamp_ns, payload);
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

}  // namespace mapmind::vlp
