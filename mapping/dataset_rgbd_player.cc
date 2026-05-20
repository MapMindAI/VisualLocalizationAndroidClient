#include "mapping/common/pose_types.h"
#include "mapping/common/utils.h"
#include "mapping/backend/simple_websocket_server.h"
#include "mapping/voxblox/voxblox_processor.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

DEFINE_string(dataset_dir, "", "Dataset folder containing rgb.csv, depth.csv, pose.csv.");
DEFINE_string(rgb_csv, "", "CSV file: timestamp_ns,path_to_rgb_image");
DEFINE_string(depth_csv, "", "CSV file: timestamp_ns,path_to_depth_image");
DEFINE_string(pose_csv, "", "CSV file: timestamp_ns,qx,qy,qz,qw,tx,ty,tz");
DEFINE_double(speed, 1.0, "Replay speed. 1.0 = realtime, 2.0 = 2x.");
DEFINE_bool(loop, false, "Loop replay.");
DEFINE_double(depth_vis_max_m, 5.0, "Max depth (m) for visualization normalization.");
DEFINE_double(depth_scale, 0.001, "Scale for integer depth image to meters.");
DEFINE_string(window_view, "RGBD Replay", "OpenCV window name for combined RGBD view.");
DEFINE_int32(render_max_size, 1080, "Max width/height for rendered combined image.");
DEFINE_bool(enable_opencv_viz, false, "Enable local OpenCV visualization.");
DEFINE_bool(enable_websocket, true, "Enable websocket output for web_client.html.");
DEFINE_int32(websocket_port, 9002, "Websocket server port.");
DEFINE_string(web_client_html, "mapping/backend/web_client.html", "Path to web client html.");
DEFINE_bool(enable_voxblox, true, "Enable Voxblox TSDF/ESDF integration from replay depth+pose.");
DEFINE_double(voxblox_voxel_size_m, 0.1, "Voxblox voxel size in meters.");
DEFINE_double(voxblox_max_depth_m, 2.0, "Max depth (m) used for TSDF integration.");
DEFINE_double(fx, 0.0, "Camera fx. If <= 0, infer from image width.");
DEFINE_double(fy, 0.0, "Camera fy. If <= 0, infer from image height.");
DEFINE_double(cx, -1.0, "Camera cx. If < 0, use image center.");
DEFINE_double(cy, -1.0, "Camera cy. If < 0, use image center.");

enum class EventType { kRgb, kDepth, kPose };

struct TimedEvent {
  uint64_t ts_ns = 0;
  EventType type = EventType::kRgb;
  size_t index = 0;
};

struct TimedPath {
  uint64_t ts_ns = 0;
  std::string path;
};

struct TimedPose {
  uint64_t ts_ns = 0;
  mapping::Pose pose;
};

bool ParseLineTokens(const std::string& line, std::vector<std::string>* tokens) {
  if (tokens == nullptr) return false;
  tokens->clear();
  std::stringstream ss(line);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    while (!tok.empty() && (tok.back() == '\r' || tok.back() == '\n' || tok.back() == ' ' ||
                            tok.back() == '\t')) {
      tok.pop_back();
    }
    size_t begin = 0;
    while (begin < tok.size() &&
           (tok[begin] == ' ' || tok[begin] == '\t' || tok[begin] == '\r' || tok[begin] == '\n')) {
      ++begin;
    }
    if (begin > 0) {
      tok.erase(0, begin);
    }
    tokens->push_back(tok);
  }
  return !tokens->empty();
}

bool ParseU64(const std::string& s, uint64_t* value) {
  if (value == nullptr) return false;
  try {
    *value = static_cast<uint64_t>(std::stoull(s));
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseFloat(const std::string& s, float* value) {
  if (value == nullptr) return false;
  try {
    *value = std::stof(s);
    return true;
  } catch (...) {
    return false;
  }
}

bool ReadTimedPathCsv(const std::string& csv_path, std::vector<TimedPath>* rows) {
  if (rows == nullptr) return false;
  rows->clear();
  std::ifstream ifs(csv_path);
  if (!ifs.is_open()) {
    LOG(ERROR) << "Cannot open: " << csv_path;
    return false;
  }
  std::string line;
  const fs::path csv_dir = fs::absolute(fs::path(csv_path)).parent_path();
  std::vector<std::string> tokens;
  int line_no = 0;
  while (std::getline(ifs, line)) {
    ++line_no;
    if (line.empty() || line[0] == '#') continue;
    if (!ParseLineTokens(line, &tokens) || tokens.size() < 2) {
      LOG(WARNING) << "Skip invalid line " << line_no << " in " << csv_path;
      continue;
    }
    uint64_t ts_ns = 0;
    if (!ParseU64(tokens[0], &ts_ns)) {
      LOG(WARNING) << "Skip line " << line_no << " (invalid timestamp) in " << csv_path;
      continue;
    }
    fs::path img_path(tokens[1]);
    if (img_path.is_relative()) {
      img_path = csv_dir / img_path;
    }
    rows->push_back(TimedPath{ts_ns, img_path.lexically_normal().string()});
  }
  std::sort(rows->begin(), rows->end(), [](const TimedPath& a, const TimedPath& b) {
    return a.ts_ns < b.ts_ns;
  });
  return true;
}

bool ReadTimedPoseCsv(const std::string& csv_path, std::vector<TimedPose>* rows) {
  if (rows == nullptr) return false;
  rows->clear();
  std::ifstream ifs(csv_path);
  if (!ifs.is_open()) {
    LOG(ERROR) << "Cannot open: " << csv_path;
    return false;
  }

  Eigen::Quaternionf q_offset(
      Eigen::AngleAxisf(-static_cast<float>(M_PI) / 2.0f, Eigen::Vector3f::UnitX()));
  mapping::Pose offset(q_offset, Eigen::Vector3f(0.0f, 0.0f, 0.0f));

  std::string line;
  std::vector<std::string> tokens;
  int line_no = 0;
  while (std::getline(ifs, line)) {
    ++line_no;
    if (line.empty() || line[0] == '#') continue;
    if (!ParseLineTokens(line, &tokens) || tokens.size() < 8) {
      LOG(WARNING) << "Skip invalid line " << line_no << " in " << csv_path;
      continue;
    }
    uint64_t ts_ns = 0;
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f, tx = 0.0f, ty = 0.0f, tz = 0.0f;
    if (!ParseU64(tokens[0], &ts_ns) || !ParseFloat(tokens[1], &qx) ||
        !ParseFloat(tokens[2], &qy) || !ParseFloat(tokens[3], &qz) ||
        !ParseFloat(tokens[4], &qw) || !ParseFloat(tokens[5], &tx) ||
        !ParseFloat(tokens[6], &ty) || !ParseFloat(tokens[7], &tz)) {
      LOG(WARNING) << "Skip invalid numeric line " << line_no << " in " << csv_path;
      continue;
    }
    Eigen::Quaternionf q(qw, qx, qy, qz);
    if (q.norm() < 1e-8f) {
      q = Eigen::Quaternionf::Identity();
    } else {
      q.normalize();
    }
    mapping::Pose pose_raw(q, Eigen::Vector3f(tx, ty, tz));

    // transform the coordinate of the pose
    rows->push_back(TimedPose{ts_ns, offset * pose_raw});
  }
  std::sort(rows->begin(), rows->end(), [](const TimedPose& a, const TimedPose& b) {
    return a.ts_ns < b.ts_ns;
  });
  return true;
}

cv::Mat DepthToMeters(const cv::Mat& depth_raw, float scale) {
  if (depth_raw.empty()) return cv::Mat();
  if (depth_raw.type() == CV_32F) return depth_raw;
  if (depth_raw.type() == CV_16U || depth_raw.type() == CV_16S || depth_raw.type() == CV_32S) {
    cv::Mat depth_m;
    depth_raw.convertTo(depth_m, CV_32F, static_cast<double>(scale));
    return depth_m;
  }
  if (depth_raw.type() == CV_8U) {
    cv::Mat depth_m;
    depth_raw.convertTo(depth_m, CV_32F, static_cast<double>(scale));
    return depth_m;
  }
  return cv::Mat();
}

cv::Mat ColorizeDepth(const cv::Mat& depth_m, float max_m) {
  if (depth_m.empty() || depth_m.type() != CV_32F) return cv::Mat();
  cv::Mat clipped = depth_m.clone();
  const float hi = std::max(0.1f, max_m);
  cv::threshold(clipped, clipped, hi, hi, cv::THRESH_TRUNC);
  cv::Mat norm;
  clipped.convertTo(norm, CV_32F, 255.0f / hi);
  cv::Mat u8;
  norm.convertTo(u8, CV_8U);
  cv::Mat color;
  cv::applyColorMap(u8, color, cv::COLORMAP_TURBO);
  return color;
}

cv::Mat ResizeToMaxSize(const cv::Mat& img, int max_size) {
  if (img.empty()) return img;
  const int limit = std::max(1, max_size);
  const int w = img.cols;
  const int h = img.rows;
  if (w <= limit && h <= limit) return img;
  const double scale = std::min(static_cast<double>(limit) / static_cast<double>(w),
                                static_cast<double>(limit) / static_cast<double>(h));
  cv::Mat out;
  cv::resize(img, out, cv::Size(), scale, scale, cv::INTER_AREA);
  return out;
}

void BuildEvents(const std::vector<TimedPath>& rgbs, const std::vector<TimedPath>& depths,
                 const std::vector<TimedPose>& poses, std::vector<TimedEvent>* events) {
  events->clear();
  events->reserve(rgbs.size() + depths.size() + poses.size());
  for (size_t i = 0; i < rgbs.size(); ++i) events->push_back(TimedEvent{rgbs[i].ts_ns, EventType::kRgb, i});
  for (size_t i = 0; i < depths.size(); ++i) events->push_back(TimedEvent{depths[i].ts_ns, EventType::kDepth, i});
  for (size_t i = 0; i < poses.size(); ++i) events->push_back(TimedEvent{poses[i].ts_ns, EventType::kPose, i});
  std::sort(events->begin(), events->end(),
            [](const TimedEvent& a, const TimedEvent& b) { return a.ts_ns < b.ts_ns; });
}

}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  std::string rgb_csv = FLAGS_rgb_csv;
  std::string depth_csv = FLAGS_depth_csv;
  std::string pose_csv = FLAGS_pose_csv;
  if (!FLAGS_dataset_dir.empty()) {
    const fs::path root = fs::path(FLAGS_dataset_dir);
    if (rgb_csv.empty()) rgb_csv = (root / "rgb.csv").string();
    if (depth_csv.empty()) depth_csv = (root / "depth.csv").string();
    if (pose_csv.empty()) pose_csv = (root / "pose.csv").string();
  }

  if (rgb_csv.empty() || depth_csv.empty() || pose_csv.empty()) {
    LOG(ERROR) << "Required: --dataset_dir or all of --rgb_csv --depth_csv --pose_csv";
    return 1;
  }

  std::vector<TimedPath> rgb_rows;
  std::vector<TimedPath> depth_rows;
  std::vector<TimedPose> pose_rows;
  if (!ReadTimedPathCsv(rgb_csv, &rgb_rows) || !ReadTimedPathCsv(depth_csv, &depth_rows) ||
      !ReadTimedPoseCsv(pose_csv, &pose_rows)) {
    return 1;
  }
  if (rgb_rows.empty() && depth_rows.empty() && pose_rows.empty()) {
    LOG(ERROR) << "No valid rows loaded.";
    return 1;
  }

  std::vector<TimedEvent> events;
  BuildEvents(rgb_rows, depth_rows, pose_rows, &events);

  std::unique_ptr<vlpweb::SimpleWebsocketServer> web_server;
  if (FLAGS_enable_websocket) {
    web_server = std::make_unique<vlpweb::SimpleWebsocketServer>(FLAGS_websocket_port,
                                                                  FLAGS_web_client_html);
    web_server->Start();
    LOG(INFO) << "Open web client: http://127.0.0.1:" << FLAGS_websocket_port << "/index.html";
  }

  if (FLAGS_enable_opencv_viz) {
    cv::namedWindow(FLAGS_window_view, cv::WINDOW_NORMAL);
  }

  const double speed = std::max(1e-3, FLAGS_speed);
  size_t loop_count = 0;
  cv::Mat latest_rgb;
  cv::Mat latest_depth_vis;
  mapping::Pose latest_pose = mapping::Pose();
  bool has_pose = false;
  std::deque<std::array<float, 3>> trajectory;
  std::vector<mapping::VoxbloxProcessor::VizPoint> esdf_viz_points;
  bool esdf_points_updated = false;
  int frame_id = 0;
  auto last_send = std::chrono::steady_clock::now();
  std::unique_ptr<mapping::VoxbloxProcessor> voxblox_processor;
  if (FLAGS_enable_voxblox) {
    mapping::VoxbloxProcessor::Config cfg;
    cfg.voxel_size_m = static_cast<float>(FLAGS_voxblox_voxel_size_m);
    cfg.max_depth_m = static_cast<float>(FLAGS_voxblox_max_depth_m);
    voxblox_processor = std::make_unique<mapping::VoxbloxProcessor>(cfg);
  }

  do {
    ++loop_count;
    LOG(INFO) << "Replay loop " << loop_count << " with " << events.size() << " events.";
    uint64_t prev_ts_ns = events.front().ts_ns;
    for (size_t i = 0; i < events.size(); ++i) {
      const TimedEvent& e = events[i];
      const uint64_t dt_ns = (i == 0 || e.ts_ns < prev_ts_ns) ? 0 : (e.ts_ns - prev_ts_ns);
      prev_ts_ns = e.ts_ns;
      if (dt_ns > 0) {
        const double dt_s = static_cast<double>(dt_ns) * 1e-9 / speed;
        std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<int64_t>(dt_s * 1e6)));
      }

      if (e.type == EventType::kRgb) {
        const cv::Mat img = cv::imread(rgb_rows[e.index].path, cv::IMREAD_COLOR);
        if (!img.empty()) latest_rgb = img;
      } else if (e.type == EventType::kDepth) {
        const cv::Mat raw = cv::imread(depth_rows[e.index].path, cv::IMREAD_UNCHANGED);
        const cv::Mat depth_m = DepthToMeters(raw, static_cast<float>(FLAGS_depth_scale));
        if (!depth_m.empty()) {
          latest_depth_vis = ColorizeDepth(depth_m, static_cast<float>(FLAGS_depth_vis_max_m));
          if (voxblox_processor && has_pose) {
            const float fx =
                FLAGS_fx > 0.0 ? static_cast<float>(FLAGS_fx) : static_cast<float>(depth_m.cols);
            const float fy =
                FLAGS_fy > 0.0 ? static_cast<float>(FLAGS_fy) : static_cast<float>(depth_m.rows);
            const float cx = FLAGS_cx >= 0.0 ? static_cast<float>(FLAGS_cx)
                                             : static_cast<float>(depth_m.cols - 1) * 0.5f;
            const float cy = FLAGS_cy >= 0.0 ? static_cast<float>(FLAGS_cy)
                                             : static_cast<float>(depth_m.rows - 1) * 0.5f;
            if (voxblox_processor->Integrate(depth_m, latest_pose, fx, fy, cx, cy)) {
              voxblox_processor->GetEsdfVisualization(&esdf_viz_points);
              esdf_points_updated = true;
            }
          }
        }
      } else {
        latest_pose = pose_rows[e.index].pose;
        has_pose = true;
      }

      if (!latest_depth_vis.empty()) {
        cv::Mat rgb_show;
        if (!latest_rgb.empty()) {
          rgb_show = latest_rgb.clone();
        } else {
          rgb_show = cv::Mat::zeros(latest_depth_vis.size(), CV_8UC3);
          cv::putText(rgb_show, "No RGB", cv::Point(20, 40), cv::FONT_HERSHEY_SIMPLEX, 1.0,
                      cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
        }
        cv::Mat depth_show = latest_depth_vis.clone();
        if (depth_show.size() != rgb_show.size()) {
          cv::resize(depth_show, depth_show, rgb_show.size(), 0.0, 0.0, cv::INTER_LINEAR);
        }
        const auto t = latest_pose.translation();
        cv::putText(rgb_show,
                    cv::format("pose t=(%.3f, %.3f, %.3f) ts=%llu", t.x(), t.y(), t.z(),
                               static_cast<unsigned long long>(e.ts_ns)),
                    cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.8,
                    cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
        cv::Mat combined;
        cv::hconcat(rgb_show, depth_show, combined);
        cv::Mat render = ResizeToMaxSize(combined, FLAGS_render_max_size);
        if (FLAGS_enable_opencv_viz) {
          cv::imshow(FLAGS_window_view, render);
        }

        trajectory.push_back({t.x(), t.y(), t.z()});
        while (trajectory.size() > 1200) {
          trajectory.pop_front();
        }

        if (web_server) {
          std::vector<uint8_t> img_buf;
          cv::imencode(".jpg", render, img_buf, {cv::IMWRITE_JPEG_QUALITY, 80});
          const std::string img_b64 = vlputil::Base64Encode(img_buf.data(), img_buf.size());

          const auto now_send = std::chrono::steady_clock::now();
          const double dt = std::chrono::duration<double>(now_send - last_send).count();
          const double fps = dt > 1e-6 ? 1.0 / dt : 0.0;
          last_send = now_send;
          ++frame_id;

          std::ostringstream oss;
          oss << "{";
          oss << "\"type\":\"update\",";
          oss << "\"frame_id\":" << frame_id << ",";
          oss << "\"image_jpeg_b64\":\"" << img_b64 << "\",";
          oss << "\"fps\":" << fps << ",";
          oss << "\"pose\":{"
              << "\"tx\":" << t.x() << ",\"ty\":" << t.y() << ",\"tz\":" << t.z() << "},";
          oss << "\"scale_text\":\"dataset replay\",";
          oss << "\"da3_status\":\"offline rgbd replay\",";
          oss << "\"voxblox\":{\"enabled\":" << (voxblox_processor ? "true" : "false") << ","
              << "\"integrated_frames\":"
              << (voxblox_processor ? voxblox_processor->IntegratedFrameCount() : 0) << ","
              << "\"voxel_size_m\":"
              << (voxblox_processor ? FLAGS_voxblox_voxel_size_m : 0.0) << ","
              << "\"esdf_count\":" << esdf_viz_points.size() << "},";
          oss << "\"trajectory\":[";
          for (size_t k = 0; k < trajectory.size(); ++k) {
            if (k) oss << ",";
            oss << "[" << trajectory[k][0] << "," << trajectory[k][1] << "," << trajectory[k][2]
                << "]";
          }
          oss << "]";
          if (esdf_points_updated) {
            oss << ",\"esdf_points\":[";
            for (size_t p = 0; p < esdf_viz_points.size(); ++p) {
              if (p) oss << ",";
              const auto& ep = esdf_viz_points[p];
              oss << "[" << ep.x << "," << ep.y << "," << ep.z << "," << ep.r << "," << ep.g
                  << "," << ep.b << "," << ep.v << "]";
            }
            oss << "]";
            esdf_points_updated = false;
          }
          oss << "}";
          web_server->BroadcastText(oss.str());
        }
      }
      if (FLAGS_enable_opencv_viz) {
        const int key = cv::waitKey(1);
        if (key == 27 || key == 'q' || key == 'Q') {
          cv::destroyAllWindows();
          if (web_server) web_server->Stop();
          google::ShutdownGoogleLogging();
          return 0;
        }
      }
    }
  } while (FLAGS_loop);

  if (FLAGS_enable_opencv_viz) {
    cv::destroyAllWindows();
  }
  if (web_server) {
    web_server->Stop();
  }
  google::ShutdownGoogleLogging();
  return 0;
}
