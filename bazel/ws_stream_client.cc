#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/sync_stream.h>

#include "da3_onnx_runner.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kFrameMagic = 0x564C5032U;  // "VLP2"
constexpr size_t kFrameHeaderSize = 64;
constexpr char kStreamMethod[] = "/vlp.FrameStreamService/StreamFrames";

using FramePose = da3client::FramePose;
using Keyframe = da3client::Keyframe;
using Da3Output = da3client::Da3Output;

struct FramePacket {
  uint64_t timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  float fx = 0.0f;
  float fy = 0.0f;
  float cx = 0.0f;
  float cy = 0.0f;
  FramePose pose;
  std::vector<uint8_t> jpeg_bytes;
};

DEFINE_string(host, "127.0.0.1", "gRPC server host.");
DEFINE_int32(port, 50051, "gRPC server port.");
DEFINE_int32(window_width, 640, "Display window width.");
DEFINE_int32(window_height, 480, "Display window height.");
DEFINE_string(da3_model, "python/models/da3_small_2_392x224_sim.onnx", "Path to DA3 ONNX model.");
DEFINE_int32(da3_width, 392, "DA3 model input width.");
DEFINE_int32(da3_height, 224, "DA3 model input height.");
DEFINE_double(keyframe_rot_deg, 6.0, "Keyframe threshold: rotation delta in degrees.");
DEFINE_double(keyframe_trans_m, 0.12, "Keyframe threshold: translation delta in meters.");

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64LE(const uint8_t* p) {
  return static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) |
         (static_cast<uint64_t>(p[2]) << 16) | (static_cast<uint64_t>(p[3]) << 24) |
         (static_cast<uint64_t>(p[4]) << 32) | (static_cast<uint64_t>(p[5]) << 40) |
         (static_cast<uint64_t>(p[6]) << 48) | (static_cast<uint64_t>(p[7]) << 56);
}

float ReadF32LE(const uint8_t* p) {
  const uint32_t bits = ReadU32LE(p);
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(float));
  return out;
}

double TransDelta(const FramePose& a, const FramePose& b) {
  const double dx = static_cast<double>(b.tx) - static_cast<double>(a.tx);
  const double dy = static_cast<double>(b.ty) - static_cast<double>(a.ty);
  const double dz = static_cast<double>(b.tz) - static_cast<double>(a.tz);
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double QuatAngleDeg(const FramePose& a, const FramePose& b) {
  std::array<double, 4> q1 = {
      static_cast<double>(a.qx), static_cast<double>(a.qy),
      static_cast<double>(a.qz), static_cast<double>(a.qw)};
  std::array<double, 4> q2 = {
      static_cast<double>(b.qx), static_cast<double>(b.qy),
      static_cast<double>(b.qz), static_cast<double>(b.qw)};
  auto norm4 = [](const std::array<double, 4>& q) {
    return std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  };
  const double n1 = norm4(q1);
  const double n2 = norm4(q2);
  if (n1 <= 1e-12 || n2 <= 1e-12) {
    return 0.0;
  }
  for (int i = 0; i < 4; ++i) {
    q1[i] /= n1;
    q2[i] /= n2;
  }
  const double dot = std::clamp(
      std::abs(q1[0] * q2[0] + q1[1] * q2[1] + q1[2] * q2[2] + q1[3] * q2[3]),
      -1.0, 1.0);
  const double angle_rad = 2.0 * std::acos(dot);
  return angle_rad * 180.0 / M_PI;
}

std::optional<FramePacket> ParseFramePayload(const std::string& payload) {
  if (payload.size() < kFrameHeaderSize) {
    return std::nullopt;
  }
  const auto* data = reinterpret_cast<const uint8_t*>(payload.data());
  const uint32_t magic = ReadU32LE(data + 0);
  if (magic != kFrameMagic) {
    return std::nullopt;
  }

  FramePacket packet;
  packet.timestamp_ns = ReadU64LE(data + 4);
  packet.width = ReadU32LE(data + 12);
  packet.height = ReadU32LE(data + 16);
  packet.fx = ReadF32LE(data + 20);
  packet.fy = ReadF32LE(data + 24);
  packet.cx = ReadF32LE(data + 28);
  packet.cy = ReadF32LE(data + 32);
  packet.pose.qx = ReadF32LE(data + 36);
  packet.pose.qy = ReadF32LE(data + 40);
  packet.pose.qz = ReadF32LE(data + 44);
  packet.pose.qw = ReadF32LE(data + 48);
  packet.pose.tx = ReadF32LE(data + 52);
  packet.pose.ty = ReadF32LE(data + 56);
  packet.pose.tz = ReadF32LE(data + 60);

  packet.jpeg_bytes.assign(data + kFrameHeaderSize, data + payload.size());
  if (packet.jpeg_bytes.empty()) {
    return std::nullopt;
  }
  return packet;
}

std::string ByteBufferToString(grpc::ByteBuffer* buffer) {
  std::vector<grpc::Slice> slices;
  const grpc::Status dump_status = buffer->Dump(&slices);
  if (!dump_status.ok()) {
    return {};
  }
  size_t total = 0;
  for (const auto& slice : slices) {
    total += slice.size();
  }
  std::string out;
  out.reserve(total);
  for (const auto& slice : slices) {
    out.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
  }
  return out;
}

void DrawOverlay(cv::Mat& image_bgr, const FramePacket& packet, double fps) {
  const int pp_x = static_cast<int>(std::lround(packet.cx));
  const int pp_y = static_cast<int>(std::lround(packet.cy));
  if (0 <= pp_x && pp_x < image_bgr.cols && 0 <= pp_y && pp_y < image_bgr.rows) {
    cv::drawMarker(
        image_bgr, cv::Point(pp_x, pp_y), cv::Scalar(0, 255, 255), cv::MARKER_CROSS, 24, 2);
    cv::circle(image_bgr, cv::Point(pp_x, pp_y), 12, cv::Scalar(0, 255, 255), 1);
  }

  const std::vector<std::string> lines = {
      "ts(ns): " + std::to_string(packet.timestamp_ns),
      cv::format("fps: %.2f", fps),
      cv::format("intrinsics fx=%.2f fy=%.2f cx=%.2f cy=%.2f", packet.fx, packet.fy, packet.cx,
                 packet.cy),
      cv::format("pose q=(%.4f, %.4f, %.4f, %.4f)", packet.pose.qx, packet.pose.qy,
                 packet.pose.qz, packet.pose.qw),
      cv::format("pose t=(%.4f, %.4f, %.4f)", packet.pose.tx, packet.pose.ty, packet.pose.tz),
      "press q to quit",
  };

  int y = 28;
  for (const auto& line : lines) {
    cv::putText(image_bgr, line, cv::Point(10, y), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    y += 26;
  }
}

class Da3Worker {
 public:
  explicit Da3Worker(std::unique_ptr<da3client::Da3OnnxRunner> runner)
      : runner_(std::move(runner)), worker_(&Da3Worker::ThreadMain, this) {
    std::lock_guard<std::mutex> lock(mu_);
    last_status_ = "DA3: waiting for first keyframe pair";
  }

  ~Da3Worker() {
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

  bool IsReady() const { return runner_ != nullptr && runner_->IsReady(); }

  std::string ErrorMessage() const {
    if (!runner_) {
      return "DA3 runner missing";
    }
    return runner_->ErrorMessage();
  }

  void Submit(const Keyframe& a, const Keyframe& b) {
    std::lock_guard<std::mutex> lock(mu_);
    jobs_.clear();
    jobs_.push_back({a, b});
    last_status_ = cv::format("DA3: queued (kf%d,kf%d)", a.idx, b.idx);
    cv_.notify_one();
  }

  std::optional<Da3Output> GetLatestOutput() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!latest_output_.has_value()) {
      return std::nullopt;
    }
    Da3Output out;
    out.scale_text = latest_output_->scale_text;
    out.pair_label = latest_output_->pair_label;
    out.depth_vis = latest_output_->depth_vis.clone();
    return out;
  }

  std::string GetLatestStatus() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_status_;
  }

 private:
  struct Job {
    Keyframe a;
    Keyframe b;
  };

  void ThreadMain() {
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
      latest_output_ = std::move(out);
      last_status_ = cv::format("DA3: ok (kf%d,kf%d)", job.a.idx, job.b.idx);
    }
  }

  std::unique_ptr<da3client::Da3OnnxRunner> runner_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::optional<Da3Output> latest_output_;
  std::string last_status_;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::SetUsageMessage(
      "ws_stream_client --host=127.0.0.1 --port=50051 "
      "--window_width=640 --window_height=480 "
      "--da3_model=python/models/da3_small_2_392x224_sim.onnx");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  const std::string target = FLAGS_host + ":" + std::to_string(FLAGS_port);
  LOG(INFO) << "Connecting to " << target;

  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  grpc::ClientContext context;

  const std::string subscribe = "subscribe";
  grpc::Slice request_slice(subscribe.data(), subscribe.size());
  grpc::ByteBuffer request_buffer(&request_slice, 1);
  const grpc::internal::RpcMethod rpc_method(
      kStreamMethod, grpc::internal::RpcMethod::SERVER_STREAMING);
  std::unique_ptr<grpc::ClientReader<grpc::ByteBuffer>> reader(
      grpc::internal::ClientReaderFactory<grpc::ByteBuffer>::Create(
          channel.get(), rpc_method, &context, request_buffer));
  if (!reader) {
    LOG(ERROR) << "Failed to create streaming reader.";
    return 1;
  }

  std::unique_ptr<Da3Worker> da3_worker;
  {
    auto runner =
        std::make_unique<da3client::Da3OnnxRunner>(FLAGS_da3_model, FLAGS_da3_width, FLAGS_da3_height);
    if (!runner->IsReady()) {
      LOG(WARNING) << "[DA3] disabled: " << runner->ErrorMessage();
    } else {
      da3_worker = std::make_unique<Da3Worker>(std::move(runner));
      LOG(INFO) << "[DA3] worker started.";
    }
  }

  const std::string window_name = "VLP gRPC Stream";
  cv::namedWindow(window_name, cv::WINDOW_NORMAL);
  cv::resizeWindow(window_name, FLAGS_window_width, FLAGS_window_height);

  const auto start_time = std::chrono::steady_clock::now();
  int frame_count = 0;
  int last_log_count = 0;
  bool user_quit = false;

  std::optional<Keyframe> last_kf;
  int keyframe_count = 0;
  cv::Mat latest_depth_vis;
  std::string latest_scale_text = "scale: n/a (pose-translation)";
  std::string latest_pair_label;
  std::string latest_da3_status =
      da3_worker ? "DA3: waiting for first keyframe pair" : "DA3: disabled";

  grpc::ByteBuffer response_buffer;
  while (reader->Read(&response_buffer)) {
    const std::string payload = ByteBufferToString(&response_buffer);
    auto packet_opt = ParseFramePayload(payload);
    if (!packet_opt.has_value()) {
      continue;
    }
    FramePacket packet = std::move(packet_opt.value());
    if (packet.width == 0 || packet.height == 0) {
      continue;
    }

    cv::Mat encoded(1, static_cast<int>(packet.jpeg_bytes.size()), CV_8UC1,
                    packet.jpeg_bytes.data());
    cv::Mat image_bgr = cv::imdecode(encoded, cv::IMREAD_COLOR);
    if (image_bgr.empty()) {
      continue;
    }

    ++frame_count;
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - start_time).count();
    const double fps = elapsed > 0.0 ? frame_count / elapsed : 0.0;

    bool is_keyframe = false;
    if (!last_kf.has_value()) {
      is_keyframe = true;
    } else {
      const double rot_deg = QuatAngleDeg(last_kf->pose, packet.pose);
      const double trans_m = TransDelta(last_kf->pose, packet.pose);
      is_keyframe = rot_deg >= FLAGS_keyframe_rot_deg || trans_m >= FLAGS_keyframe_trans_m;
    }

    if (is_keyframe) {
      ++keyframe_count;
      Keyframe curr;
      curr.idx = keyframe_count;
      curr.timestamp_ns = packet.timestamp_ns;
      curr.image_bgr = image_bgr.clone();
      curr.pose = packet.pose;
      LOG(INFO) << "[KF] kf" << curr.idx << " ts=" << curr.timestamp_ns;

      if (da3_worker && last_kf.has_value()) {
        da3_worker->Submit(*last_kf, curr);
      }
      last_kf = std::move(curr);
    }

    if (da3_worker) {
      auto out = da3_worker->GetLatestOutput();
      if (out.has_value()) {
        latest_depth_vis = out->depth_vis;
        latest_scale_text = out->scale_text;
        latest_pair_label = out->pair_label;
      }
      latest_da3_status = da3_worker->GetLatestStatus();
    }

    cv::Mat overlay = image_bgr.clone();
    DrawOverlay(overlay, packet, fps);
    if (is_keyframe) {
      cv::putText(overlay, "KEYFRAME", cv::Point(10, overlay.rows - 16), cv::FONT_HERSHEY_SIMPLEX,
                  0.8, cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
    }

    cv::Mat depth_panel;
    if (latest_depth_vis.empty()) {
      depth_panel = cv::Mat::zeros(overlay.size(), overlay.type());
      cv::putText(depth_panel, "No depth yet", cv::Point(20, 40), cv::FONT_HERSHEY_SIMPLEX, 1.0,
                  cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
      cv::putText(depth_panel, latest_da3_status, cv::Point(20, 72), cv::FONT_HERSHEY_SIMPLEX,
                  0.65, cv::Scalar(180, 220, 255), 2, cv::LINE_AA);
    } else {
      cv::resize(latest_depth_vis, depth_panel, overlay.size(), 0.0, 0.0, cv::INTER_LINEAR);
      cv::putText(depth_panel, latest_da3_status, cv::Point(10, 76), cv::FONT_HERSHEY_SIMPLEX,
                  0.55, cv::Scalar(200, 255, 255), 2, cv::LINE_AA);
    }
    cv::putText(depth_panel, latest_scale_text, cv::Point(10, 24), cv::FONT_HERSHEY_SIMPLEX,
                0.6, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
    if (!latest_pair_label.empty()) {
      cv::putText(depth_panel, latest_pair_label, cv::Point(10, 50), cv::FONT_HERSHEY_SIMPLEX,
                  0.6, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
    }

    cv::Mat combined;
    cv::hconcat(overlay, depth_panel, combined);
    cv::imshow(window_name, combined);

    const int key = cv::waitKey(1) & 0xFF;
    if (key == 'q') {
      LOG(INFO) << "Quit requested by user.";
      user_quit = true;
      break;
    }

    if (frame_count - last_log_count >= 30) {
      last_log_count = frame_count;
      LOG(INFO) << "[" << frame_count << "] stream_fps=" << cv::format("%.2f", fps)
                << " ts=" << packet.timestamp_ns << " size=" << packet.width << "x"
                << packet.height;
    }
  }

  if (user_quit) {
    context.TryCancel();
  }
  const grpc::Status status = reader->Finish();
  if (!status.ok() && !user_quit) {
    LOG(ERROR) << "gRPC stream finished with error: " << status.error_code() << " "
               << status.error_message();
  }

  cv::destroyAllWindows();
  google::ShutdownGoogleLogging();
  return 0;
}
