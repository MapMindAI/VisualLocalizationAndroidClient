#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/sync_stream.h>

#include "da3_onnx_runner.h"
#include "frame_protocol.h"
#include "render_overlay.h"
#include "simple_websocket_server.h"
#include "utils.h"

#include <Eigen/Geometry>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr char kStreamMethod[] = "/vlp.FrameStreamService/StreamFrames";

using Keyframe = da3client::Keyframe;

DEFINE_string(host, "127.0.0.1", "gRPC server host.");
DEFINE_int32(port, 50051, "gRPC server port.");
DEFINE_int32(window_width, 640, "Display window width.");
DEFINE_int32(window_height, 480, "Display window height.");
DEFINE_string(da3_model, "python/models/da3_small_2_392x224_sim.onnx", "Path to DA3 ONNX model.");
DEFINE_int32(da3_width, 392, "DA3 model input width.");
DEFINE_int32(da3_height, 224, "DA3 model input height.");
DEFINE_double(keyframe_rot_deg, 6.0, "Keyframe threshold: rotation delta in degrees.");
DEFINE_double(keyframe_trans_m, 0.12, "Keyframe threshold: translation delta in meters.");
DEFINE_bool(enable_websocket, true, "Enable websocket stream server.");
DEFINE_int32(websocket_port, 9002, "Websocket server port.");
DEFINE_int32(web_depth_history, 5, "Recent depth clouds kept for web map.");
DEFINE_int32(web_depth_step, 6, "Pixel stride when generating web depth cloud.");
DEFINE_string(web_client_html, "bazel/web_client.html", "Path to web client html.");
DEFINE_double(web_send_hz, 10.0, "Websocket broadcast rate.");
DEFINE_int32(web_max_traj_points, 1200, "Max trajectory points sent to browser.");
DEFINE_int32(web_max_cloud_points, 12000, "Max points per depth cloud sent to browser.");

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

  std::unique_ptr<da3client::Da3Worker> da3_worker;
  std::unique_ptr<vlpweb::SimpleWebsocketServer> web_server;
  {
    auto runner =
        std::make_unique<da3client::Da3OnnxRunner>(FLAGS_da3_model, FLAGS_da3_width, FLAGS_da3_height);
    if (!runner->IsReady()) {
      LOG(WARNING) << "[DA3] disabled: " << runner->ErrorMessage();
    } else {
      da3_worker = std::make_unique<da3client::Da3Worker>(std::move(runner));
      LOG(INFO) << "[DA3] worker started.";
    }
  }

  const std::string window_name = "VLP gRPC Stream";
  cv::namedWindow(window_name, cv::WINDOW_NORMAL);
  cv::resizeWindow(window_name, FLAGS_window_width, FLAGS_window_height);
  if (FLAGS_enable_websocket) {
    web_server = std::make_unique<vlpweb::SimpleWebsocketServer>(FLAGS_websocket_port,
                                                                  FLAGS_web_client_html);
    web_server->Start();
    LOG(INFO) << "Open web client: http://127.0.0.1:" << FLAGS_websocket_port
              << "/index.html";
  }

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
  std::deque<std::array<float, 3>> trajectory;
  std::deque<std::pair<std::string, std::vector<std::array<float, 3>>>> recent_clouds;
  std::string last_cloud_pair;
  auto last_web_send = std::chrono::steady_clock::now();

  grpc::ByteBuffer response_buffer;
  while (reader->Read(&response_buffer)) {
    const std::string payload = vlputil::ByteBufferToString(&response_buffer);
    auto packet_opt = vlpstream::ParseFramePayload(payload);
    if (!packet_opt.has_value()) {
      continue;
    }
    vlpstream::FramePacket packet = std::move(packet_opt.value());
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
      const double rot_deg = vlputil::QuatAngleDeg(last_kf->pose, packet.pose);
      const double trans_m = vlputil::TransDelta(last_kf->pose, packet.pose);
      is_keyframe = rot_deg >= FLAGS_keyframe_rot_deg || trans_m >= FLAGS_keyframe_trans_m;
    }

    if (is_keyframe) {
      ++keyframe_count;
      Keyframe curr;
      curr.idx = keyframe_count;
      curr.timestamp_ns = packet.timestamp_ns;
      curr.image_bgr = image_bgr.clone();
      curr.pose = packet.pose;
      curr.fx = packet.fx;
      curr.fy = packet.fy;
      curr.cx = packet.cx;
      curr.cy = packet.cy;
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
    vlprender::DrawOverlay(overlay, packet, fps);
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

    trajectory.push_back({packet.pose.tx, packet.pose.ty, packet.pose.tz});
    while (trajectory.size() > 5000) {
      trajectory.pop_front();
    }
    if (da3_worker) {
      auto out = da3_worker->GetLatestOutput();
      if (out.has_value() && !out->depth_metric.empty() && out->pair_label != last_cloud_pair) {
        const int step = std::max(1, FLAGS_web_depth_step);
        const Sophus::SE3d T_w_c = vlputil::PoseToSE3(out->pose);
        const Eigen::Matrix3d R = T_w_c.so3().matrix();
        const Eigen::Vector3d t = T_w_c.translation();
        std::vector<std::array<float, 3>> cloud;
        cloud.reserve(static_cast<size_t>(out->depth_metric.rows / step) *
                      static_cast<size_t>(out->depth_metric.cols / step));
        for (int v = 0; v < out->depth_metric.rows; v += step) {
          const float* row = out->depth_metric.ptr<float>(v);
          for (int u = 0; u < out->depth_metric.cols; u += step) {
            const float z = row[u];
            if (!std::isfinite(z) || z <= 0.05f || z > 50.0f || out->fx <= 0.0f || out->fy <= 0.0f) {
              continue;
            }
            const float x = (static_cast<float>(u) - out->cx) * z / out->fx;
            const float y = (static_cast<float>(v) - out->cy) * z / out->fy;
            const Eigen::Vector3d pw =
                R * Eigen::Vector3d(static_cast<double>(x), static_cast<double>(y),
                                    static_cast<double>(z)) +
                t;
            const float wx = static_cast<float>(pw.x());
            const float wy = static_cast<float>(pw.y());
            const float wz = static_cast<float>(pw.z());
            cloud.push_back({wx, wy, wz});
          }
        }
        if (static_cast<int>(cloud.size()) > FLAGS_web_max_cloud_points) {
          const size_t stride = static_cast<size_t>(cloud.size() / FLAGS_web_max_cloud_points + 1);
          std::vector<std::array<float, 3>> sampled;
          sampled.reserve(FLAGS_web_max_cloud_points);
          for (size_t i = 0; i < cloud.size(); i += stride) {
            sampled.push_back(cloud[i]);
            if (static_cast<int>(sampled.size()) >= FLAGS_web_max_cloud_points) break;
          }
          cloud.swap(sampled);
        }
        recent_clouds.push_back({out->pair_label, std::move(cloud)});
        while (static_cast<int>(recent_clouds.size()) > std::max(1, FLAGS_web_depth_history)) {
          recent_clouds.pop_front();
        }
        last_cloud_pair = out->pair_label;
      }
    }

    if (web_server) {
      const auto now_send = std::chrono::steady_clock::now();
      const double min_dt = 1.0 / std::max(1e-3, FLAGS_web_send_hz);
      const double dt = std::chrono::duration<double>(now_send - last_web_send).count();
      if (dt < min_dt) {
        goto skip_web_send;
      }
      last_web_send = now_send;
      std::vector<uint8_t> img_buf;
      cv::imencode(".jpg", combined, img_buf, {cv::IMWRITE_JPEG_QUALITY, 80});
      const std::string img_b64 = vlputil::Base64Encode(img_buf.data(), img_buf.size());

      std::ostringstream oss;
      oss << "{";
      oss << "\"type\":\"update\",";
      oss << "\"frame_id\":" << frame_count << ",";
      oss << "\"image_jpeg_b64\":\"" << img_b64 << "\",";
      oss << "\"fps\":" << fps << ",";
      oss << "\"pose\":{"
          << "\"tx\":" << packet.pose.tx << ",\"ty\":" << packet.pose.ty << ",\"tz\":"
          << packet.pose.tz << "},";
      oss << "\"scale_text\":\"" << vlputil::JsonEscape(latest_scale_text) << "\",";
      oss << "\"da3_status\":\"" << vlputil::JsonEscape(latest_da3_status) << "\",";
      oss << "\"trajectory\":[";
      size_t begin_idx = 0;
      if (static_cast<int>(trajectory.size()) > FLAGS_web_max_traj_points) {
        begin_idx = trajectory.size() - static_cast<size_t>(FLAGS_web_max_traj_points);
      }
      for (size_t i = begin_idx; i < trajectory.size(); ++i) {
        if (i != begin_idx) oss << ",";
        oss << "[" << trajectory[i][0] << "," << trajectory[i][1] << "," << trajectory[i][2]
            << "]";
      }
      oss << "],\"depth_clouds\":[";
      for (size_t i = 0; i < recent_clouds.size(); ++i) {
        if (i) oss << ",";
        oss << "{\"id\":\"" << vlputil::JsonEscape(recent_clouds[i].first) << "\",\"points\":[";
        const auto& pts = recent_clouds[i].second;
        for (size_t j = 0; j < pts.size(); ++j) {
          if (j) oss << ",";
          oss << "[" << pts[j][0] << "," << pts[j][1] << "," << pts[j][2] << "]";
        }
        oss << "]}";
      }
      oss << "]}";
      web_server->BroadcastText(oss.str());
    }
skip_web_send:;

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
  if (web_server) {
    web_server->Stop();
  }
  google::ShutdownGoogleLogging();
  return 0;
}
