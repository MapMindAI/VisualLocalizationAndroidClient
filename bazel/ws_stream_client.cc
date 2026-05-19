#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/sync_stream.h>

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kFrameMagic = 0x564C5032U;  // "VLP2"
constexpr size_t kFrameHeaderSize = 64;
constexpr char kStreamMethod[] = "/vlp.FrameStreamService/StreamFrames";

struct FramePacket {
  uint64_t timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  float fx = 0.0f;
  float fy = 0.0f;
  float cx = 0.0f;
  float cy = 0.0f;
  float qx = 0.0f;
  float qy = 0.0f;
  float qz = 0.0f;
  float qw = 1.0f;
  float tx = 0.0f;
  float ty = 0.0f;
  float tz = 0.0f;
  std::vector<uint8_t> jpeg_bytes;
};

struct Args {
  std::string host = "127.0.0.1";
  int port = 50051;
  int window_width = 640;
  int window_height = 480;
};

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
  packet.qx = ReadF32LE(data + 36);
  packet.qy = ReadF32LE(data + 40);
  packet.qz = ReadF32LE(data + 44);
  packet.qw = ReadF32LE(data + 48);
  packet.tx = ReadF32LE(data + 52);
  packet.ty = ReadF32LE(data + 56);
  packet.tz = ReadF32LE(data + 60);

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
      cv::format("pose q=(%.4f, %.4f, %.4f, %.4f)", packet.qx, packet.qy, packet.qz, packet.qw),
      cv::format("pose t=(%.4f, %.4f, %.4f)", packet.tx, packet.ty, packet.tz),
      "press q to quit",
  };

  int y = 28;
  for (const auto& line : lines) {
    cv::putText(image_bgr, line, cv::Point(10, y), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    y += 26;
  }
}

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--host") {
      args.host = need_value("--host");
    } else if (arg == "--port") {
      args.port = std::stoi(need_value("--port"));
    } else if (arg == "--window-width") {
      args.window_width = std::stoi(need_value("--window-width"));
    } else if (arg == "--window-height") {
      args.window_height = std::stoi(need_value("--window-height"));
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "Usage: ws_stream_client [--host 127.0.0.1] [--port 50051] "
             "[--window-width 640] [--window-height 480]\n";
      std::exit(0);
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      std::exit(2);
    }
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);
  const std::string target = args.host + ":" + std::to_string(args.port);
  std::cout << "Connecting to " << target << "\n";

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
    std::cerr << "Failed to create streaming reader.\n";
    return 1;
  }

  const std::string window_name = "VLP gRPC Stream";
  cv::namedWindow(window_name, cv::WINDOW_NORMAL);
  cv::resizeWindow(window_name, args.window_width, args.window_height);

  const auto start_time = std::chrono::steady_clock::now();
  int frame_count = 0;
  int last_log_count = 0;
  bool user_quit = false;

  grpc::ByteBuffer response_buffer;
  while (reader->Read(&response_buffer)) {
    const std::string payload = ByteBufferToString(&response_buffer);
    auto packet_opt = ParseFramePayload(payload);
    if (!packet_opt.has_value()) {
      continue;
    }
    const FramePacket& packet = packet_opt.value();
    if (packet.width == 0 || packet.height == 0) {
      continue;
    }

    cv::Mat encoded(1, static_cast<int>(packet.jpeg_bytes.size()), CV_8UC1,
                    const_cast<uint8_t*>(packet.jpeg_bytes.data()));
    cv::Mat image_bgr = cv::imdecode(encoded, cv::IMREAD_COLOR);
    if (image_bgr.empty()) {
      continue;
    }

    ++frame_count;
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - start_time).count();
    const double fps = elapsed > 0.0 ? frame_count / elapsed : 0.0;

    DrawOverlay(image_bgr, packet, fps);
    cv::imshow(window_name, image_bgr);

    const int key = cv::waitKey(1) & 0xFF;
    if (key == 'q') {
      std::cout << "Quit requested by user.\n";
      user_quit = true;
      break;
    }

    if (frame_count - last_log_count >= 30) {
      last_log_count = frame_count;
      std::cout << "[" << frame_count << "] stream_fps=" << cv::format("%.2f", fps)
                << " ts=" << packet.timestamp_ns << " size=" << packet.width << "x"
                << packet.height << "\n";
    }
  }

  if (user_quit) {
    context.TryCancel();
  }
  const grpc::Status status = reader->Finish();
  if (!status.ok()) {
    std::cerr << "gRPC stream finished with error: " << status.error_code() << " "
              << status.error_message() << "\n";
  }

  cv::destroyAllWindows();
  return status.ok() ? 0 : 1;
}
