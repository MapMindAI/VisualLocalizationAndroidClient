#include "mapping/backend/simple_websocket_server.h"
#include "mapping/common/data_session.h"
#include "mapping/common/utils.h"
#include "mapping/voxblox/voxblox_processor.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

DEFINE_string(data_session, "", "Path to VLPREC1 recording file.");
DEFINE_bool(loop_session, false, "Loop replay when end-of-file is reached.");
DEFINE_bool(replay_realtime, true, "Replay using recorded relative timestamps.");
DEFINE_double(replay_speed, 1.0, "Replay speed multiplier.");

DEFINE_bool(enable_voxblox, true, "Enable Voxblox TSDF/ESDF integration from recorder depth.");
DEFINE_double(voxblox_voxel_size_m, 0.2, "Voxblox voxel size in meters.");
DEFINE_double(max_depth_m, 6.0, "Maximum depth (meters) used for depth panel visualization.");
DEFINE_int32(esdf_update_every_n, 10,
             "Update/publish ESDF points every N successful Voxblox integrations.");
DEFINE_double(esdf2d_height_m, 0.0, "World-space Y height (meters) for the 2D ESDF plane.");
DEFINE_int32(esdf2d_max_cells, 65536, "Maximum cells to publish for 2D ESDF plane.");

DEFINE_bool(enable_websocket, true, "Enable websocket stream server.");
DEFINE_int32(websocket_port, 9002, "Websocket server port.");
DEFINE_string(web_client_html, "mapping/backend/web_client.html", "Path to web client html.");
DEFINE_double(web_send_hz, 15.0, "Websocket broadcast rate.");
DEFINE_int32(web_max_traj_points, 1200, "Max trajectory points sent to browser.");

cv::Mat BuildDepthPanel(const cv::Mat& depth_m, const cv::Size& target_size,
                        const std::string& status_text) {
  cv::Mat panel;
  if (depth_m.empty()) {
    panel = cv::Mat::zeros(target_size, CV_8UC3);
    cv::putText(panel, "No recorder depth in frame", cv::Point(20, 40), cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
  } else {
    double max_val = 0.0;
    cv::minMaxLoc(depth_m, nullptr, &max_val);
    const double vis_cap = std::max(0.1, FLAGS_max_depth_m);
    const double vis_max = std::max(0.5, std::min(vis_cap, max_val));
    cv::Mat depth_8u;
    depth_m.convertTo(depth_8u, CV_8U, 255.0 / vis_max);
    cv::Mat color;
    cv::applyColorMap(depth_8u, color, cv::COLORMAP_TURBO);
    cv::resize(color, panel, target_size, 0.0, 0.0, cv::INTER_LINEAR);
  }
  cv::putText(panel, status_text, cv::Point(10, 24), cv::FONT_HERSHEY_SIMPLEX, 0.6,
              cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
  return panel;
}

}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::SetUsageMessage(
      "mono_vio_ws_stream_client --data_session=path/to/data.rec "
      "--enable_voxblox=true --enable_websocket=true");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_data_session.empty()) {
    LOG(ERROR) << "--data_session is required.";
    return 1;
  }
  if (FLAGS_replay_speed <= 0.0) {
    LOG(ERROR) << "--replay_speed must be > 0";
    return 1;
  }
  if (FLAGS_esdf_update_every_n <= 0) {
    LOG(ERROR) << "--esdf_update_every_n must be > 0";
    return 1;
  }
  if (FLAGS_max_depth_m <= 0.0) {
    LOG(ERROR) << "--max_depth_m must be > 0";
    return 1;
  }

  mapping::DataSessionReader session(FLAGS_data_session);
  if (!session.Open()) {
    return 1;
  }

  std::unique_ptr<mapping::VoxbloxProcessor> voxblox_processor;
  if (FLAGS_enable_voxblox) {
    mapping::VoxbloxProcessor::Config cfg(FLAGS_voxblox_voxel_size_m);
    cfg.max_depth_m = FLAGS_max_depth_m;
    voxblox_processor = std::make_unique<mapping::VoxbloxProcessor>(cfg);
    LOG(INFO) << "[Voxblox] enabled.";
  }

  std::unique_ptr<vlpweb::SimpleWebsocketServer> web_server;
  if (FLAGS_enable_websocket) {
    web_server = std::make_unique<vlpweb::SimpleWebsocketServer>(FLAGS_websocket_port,
                                                                  FLAGS_web_client_html);
    web_server->Start();
    LOG(INFO) << "Open web client: http://127.0.0.1:" << FLAGS_websocket_port << "/index.html";
  }

  int frame_count = 0;
  int last_log_count = 0;
  const auto start_time = std::chrono::steady_clock::now();
  std::deque<std::array<float, 3>> trajectory;
  std::vector<mapping::VoxbloxProcessor::VizPoint> esdf_viz_points;
  mapping::VoxbloxProcessor::EsdfPlane2D esdf2d_slice;
  std::vector<uint8_t> img_buf;
  img_buf.reserve(256 * 1024);
  bool esdf_points_updated = false;
  bool esdf2d_updated = false;
  auto last_web_send = std::chrono::steady_clock::now();

  int64_t last_rel_ns = -1;

  for (;;) {
    mapping::DataSessionFrame entry;
    if (!session.Next(&entry)) {
      if (!FLAGS_loop_session) {
        break;
      }
      LOG(INFO) << "Session EOF reached; reopening.";
      if (!session.Open()) {
        break;
      }
      last_rel_ns = -1;
      continue;
    }

    if (FLAGS_replay_realtime && last_rel_ns >= 0 && entry.rel_ns > last_rel_ns) {
      const int64_t dt_ns = entry.rel_ns - last_rel_ns;
      const double scaled_s = static_cast<double>(dt_ns) / 1e9 / FLAGS_replay_speed;
      const auto sleep_for = std::chrono::duration<double>(std::max(0.0, scaled_s));
      std::this_thread::sleep_for(sleep_for);
    }
    last_rel_ns = entry.rel_ns;

    vlpstream::FramePacket& packet = entry.packet;
    cv::Mat& depth_m = entry.depth_m;

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

    std::string depth_status = "depth: missing";
    if (!depth_m.empty()) {
      depth_status = "depth: ready";
      if (voxblox_processor) {
        const float sx = (packet.width > 0)
                             ? static_cast<float>(depth_m.cols) / static_cast<float>(packet.width)
                             : 1.0f;
        const float sy =
            (packet.height > 0) ? static_cast<float>(depth_m.rows) / static_cast<float>(packet.height)
                                : 1.0f;
        const float depth_fx = packet.fx * sx;
        const float depth_fy = packet.fy * sy;
        const float depth_cx = packet.cx * sx;
        const float depth_cy = packet.cy * sy;
        const bool ok = voxblox_processor->Integrate(depth_m, packet.pose, depth_fx, depth_fy,
                                                     depth_cx, depth_cy);
        if (ok) {
          const int integrated_frames = voxblox_processor->IntegratedFrameCount();
          if ((integrated_frames % FLAGS_esdf_update_every_n) == 0) {
            voxblox_processor->GetEsdfVisualization(&esdf_viz_points);
            esdf_points_updated = true;
            esdf2d_updated = voxblox_processor->GetEsdfPlaneSlice2D(
                static_cast<float>(FLAGS_esdf2d_height_m), &esdf2d_slice, FLAGS_esdf2d_max_cells);
          }
          depth_status = "depth: integrated";
        } else {
          depth_status = "depth: rejected by voxblox";
        }
      }
    }

    cv::Mat overlay = image_bgr;
    const std::vector<std::string> lines = {
        "ts(ns): " + std::to_string(packet.timestamp_ns),
        cv::format("fps: %.2f", fps),
        cv::format("intrinsics fx=%.2f fy=%.2f cx=%.2f cy=%.2f", packet.fx, packet.fy,
                   packet.cx, packet.cy),
        cv::format("pose t=(%.4f, %.4f, %.4f)", packet.pose.translation().x(),
                   packet.pose.translation().y(), packet.pose.translation().z()),
        depth_status,
    };
    int y = 28;
    for (const auto& line : lines) {
      cv::putText(overlay, line, cv::Point(10, y), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                  cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
      y += 26;
    }

    cv::Mat depth_panel = BuildDepthPanel(depth_m, overlay.size(), depth_status);
    cv::Mat combined;
    cv::vconcat(overlay, depth_panel, combined);

    trajectory.push_back(
        {packet.pose.translation().x(), packet.pose.translation().y(), packet.pose.translation().z()});
    while (trajectory.size() > 5000) {
      trajectory.pop_front();
    }

    if (web_server) {
      const auto now_send = std::chrono::steady_clock::now();
      const double min_dt = 1.0 / std::max(1e-3, FLAGS_web_send_hz);
      const double dt = std::chrono::duration<double>(now_send - last_web_send).count();
      if (dt >= min_dt) {
        last_web_send = now_send;
        img_buf.clear();
        cv::imencode(".jpg", combined, img_buf, {cv::IMWRITE_JPEG_QUALITY, 80});
        const std::string img_b64 = vlputil::Base64Encode(img_buf.data(), img_buf.size());

        std::ostringstream oss;
        oss << "{";
        oss << "\"type\":\"update\",";
        oss << "\"frame_id\":" << frame_count << ",";
        oss << "\"image_jpeg_b64\":\"" << img_b64 << "\",";
        oss << "\"fps\":" << fps << ",";
        oss << "\"pose\":{"
            << "\"tx\":" << packet.pose.translation().x() << ",\"ty\":"
            << packet.pose.translation().y() << ",\"tz\":" << packet.pose.translation().z()
            << "},";
        oss << "\"depth_status\":\"" << vlputil::JsonEscape(depth_status) << "\",";
        oss << "\"voxblox\":{"
            << "\"enabled\":" << (voxblox_processor ? "true" : "false") << ","
            << "\"integrated_frames\":"
            << (voxblox_processor ? voxblox_processor->IntegratedFrameCount() : 0) << ","
            << "\"voxel_size_m\":" << (voxblox_processor ? FLAGS_voxblox_voxel_size_m : 0.0)
            << ","
            << "\"esdf_count\":" << esdf_viz_points.size() << "},";
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
        oss << "]";

        if (esdf_points_updated) {
          oss << ",\"esdf_points\":[";
          for (size_t i = 0; i < esdf_viz_points.size(); ++i) {
            if (i) oss << ",";
            const auto& p = esdf_viz_points[i];
            oss << "[" << p.x << "," << p.y << "," << p.z << "," << p.r << "," << p.g << ","
                << p.b << "," << p.v << "]";
          }
          oss << "]";
          esdf_points_updated = false;
        }
        if (esdf2d_updated) {
          oss << ",\"esdf2d\":{";
          oss << "\"width\":" << esdf2d_slice.width << ",";
          oss << "\"height\":" << esdf2d_slice.height << ",";
          oss << "\"resolution_m\":" << esdf2d_slice.resolution_m << ",";
          oss << "\"origin_x_m\":" << esdf2d_slice.origin_x_m << ",";
          oss << "\"origin_z_m\":" << esdf2d_slice.origin_z_m << ",";
          oss << "\"plane_height_m\":" << esdf2d_slice.plane_height_m << ",";
          oss << "\"distances\":[";
          for (size_t i = 0; i < esdf2d_slice.distances.size(); ++i) {
            if (i) oss << ",";
            const float v = esdf2d_slice.distances[i];
            if (std::isfinite(v)) {
              oss << v;
            } else {
              oss << "null";
            }
          }
          oss << "]}";
          esdf2d_updated = false;
        }
        oss << "}";
        web_server->BroadcastText(oss.str());
      }
    }

    if (frame_count - last_log_count >= 30) {
      last_log_count = frame_count;
      LOG(INFO) << "[" << frame_count << "] replay_fps=" << cv::format("%.2f", fps)
                << " ts=" << packet.timestamp_ns << " size=" << packet.width << "x"
                << packet.height << " depth=" << (depth_m.empty() ? "no" : "yes");
    }
  }

  if (web_server) {
    web_server->Stop();
  }
  google::ShutdownGoogleLogging();
  return 0;
}
