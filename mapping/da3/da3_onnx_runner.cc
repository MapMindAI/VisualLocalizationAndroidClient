#include "mapping/da3/da3_onnx_runner.h"

#include <Eigen/Geometry>
#include <glog/logging.h>
#include <nlohmann/json.hpp>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace da3client {
namespace {

struct TritonTensor {
  std::vector<int64_t> shape;
  std::vector<float> data;
};

bool StartsWith(const std::string& s, const std::string& pfx) {
  return s.rfind(pfx, 0) == 0;
}

std::string StripTrailingSlash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

std::string ResolveInferUrl(const std::string& model_path_in) {
  std::string s = StripTrailingSlash(model_path_in);
  if (StartsWith(s, "http://") || StartsWith(s, "https://")) {
    if (s.find("/v2/models/") != std::string::npos || s.find("/v1/models/") != std::string::npos) {
      if (s.size() >= 6 && s.substr(s.size() - 6) == "/infer") return s;
      return s + "/infer";
    }
    return s + "/v2/models/depthanything3_trt/infer";
  }
  return {};
}


bool ParseHttpUrl(const std::string& url, std::string* host, int* port, std::string* path) {
  if (host == nullptr || port == nullptr || path == nullptr) return false;
  const std::string pfx = "http://";
  if (!StartsWith(url, pfx)) return false;
  const std::string rest = url.substr(pfx.size());
  const size_t slash = rest.find('/');
  const std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  *path = (slash == std::string::npos) ? "/" : rest.substr(slash);
  const size_t colon = hostport.rfind(':');
  if (colon == std::string::npos) {
    *host = hostport;
    *port = 80;
  } else {
    *host = hostport.substr(0, colon);
    *port = std::stoi(hostport.substr(colon + 1));
  }
  return !host->empty() && *port > 0;
}

bool HttpPostJson(const std::string& url, const std::string& body, std::string* response,
                  std::string* err) {
  if (response == nullptr) return false;
  std::string host, path;
  int port = 0;
  if (!ParseHttpUrl(url, &host, &port, &path)) {
    if (err) *err = "Invalid/unsupported URL: " + url;
    return false;
  }

  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  const std::string port_s = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || res == nullptr) {
    if (err) *err = "getaddrinfo failed for " + host + ":" + port_s;
    return false;
  }

  int fd = -1;
  for (auto* p = res; p != nullptr; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    if (err) *err = "connect failed to " + host + ":" + port_s;
    return false;
  }

  std::ostringstream req;
  req << "POST " << path << " HTTP/1.1\r\n";
  req << "Host: " << host << ":" << port << "\r\n";
  req << "Content-Type: application/json\r\n";
  req << "Connection: close\r\n";
  req << "Content-Length: " << body.size() << "\r\n\r\n";
  req << body;
  const std::string req_s = req.str();
  size_t sent = 0;
  while (sent < req_s.size()) {
    const ssize_t n = send(fd, req_s.data() + sent, req_s.size() - sent, 0);
    if (n <= 0) {
      close(fd);
      if (err) *err = "send failed";
      return false;
    }
    sent += static_cast<size_t>(n);
  }

  std::string raw;
  char buf[8192];
  while (true) {
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n == 0) break;
    if (n < 0) {
      close(fd);
      if (err) *err = "recv failed";
      return false;
    }
    raw.append(buf, static_cast<size_t>(n));
  }
  close(fd);

  const size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    if (err) *err = "invalid HTTP response";
    return false;
  }
  const std::string header = raw.substr(0, header_end);
  *response = raw.substr(header_end + 4);
  if (header.find(" 200 ") == std::string::npos) {
    if (err) {
      *err = "HTTP non-200: " + header.substr(0, std::min<size_t>(header.size(), 200));
    }
    return false;
  }
  return true;
}

std::string ShapeToString(const std::vector<int64_t>& shape) {
  std::string out = "[";
  for (size_t i = 0; i < shape.size(); ++i) {
    out += std::to_string(shape[i]);
    if (i + 1 < shape.size()) out += ",";
  }
  out += "]";
  return out;
}

cv::Mat ColorizeDepth(const cv::Mat& depth_f32) {
  if (depth_f32.empty() || depth_f32.type() != CV_32F) return {};
  double mn = 0.0;
  double mx = 0.0;
  cv::minMaxLoc(depth_f32, &mn, &mx);
  if (!std::isfinite(mn) || !std::isfinite(mx) || mx <= mn) return {};
  cv::Mat norm;
  depth_f32.convertTo(norm, CV_32F, 1.0 / (mx - mn), -mn / (mx - mn));
  cv::Mat u8;
  norm.convertTo(u8, CV_8U, 255.0);
  cv::Mat vis;
  cv::applyColorMap(u8, vis, cv::COLORMAP_INFERNO);
  return vis;
}

std::vector<float> BuildImagePairNchw(const cv::Mat& a_bgr, const cv::Mat& b_bgr, int w, int h) {
  auto one = [&](const cv::Mat& bgr) {
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    cv::Mat resized;
    cv::resize(rgb, resized, cv::Size(w, h), 0.0, 0.0, cv::INTER_LINEAR);
    cv::Mat f32;
    resized.convertTo(f32, CV_32FC3, 1.0 / 255.0);
    std::vector<cv::Mat> ch(3);
    cv::split(f32, ch);
    const size_t plane = static_cast<size_t>(h) * w;
    std::vector<float> out(plane * 3);
    std::memcpy(out.data() + 0 * plane, ch[0].data, plane * sizeof(float));
    std::memcpy(out.data() + 1 * plane, ch[1].data, plane * sizeof(float));
    std::memcpy(out.data() + 2 * plane, ch[2].data, plane * sizeof(float));
    return out;
  };

  std::vector<float> a = one(a_bgr);
  std::vector<float> b = one(b_bgr);
  std::vector<float> pair;
  pair.reserve(a.size() + b.size());
  pair.insert(pair.end(), a.begin(), a.end());
  pair.insert(pair.end(), b.begin(), b.end());
  return pair;
}

std::vector<float> BuildPairIntrinsics(const Keyframe& a, const Keyframe& b) {
  const std::vector<float> ka = {a.fx, 0.0f, a.cx, 0.0f, a.fy, a.cy, 0.0f, 0.0f, 1.0f};
  const std::vector<float> kb = {b.fx, 0.0f, b.cx, 0.0f, b.fy, b.cy, 0.0f, 0.0f, 1.0f};
  std::vector<float> out;
  out.reserve(18);
  out.insert(out.end(), ka.begin(), ka.end());
  out.insert(out.end(), kb.begin(), kb.end());
  return out;
}

std::vector<float> PoseToRt3x4RowMajor(const Pose& pose) {
  const Eigen::Matrix3f R = pose.so3().matrix();
  const Eigen::Vector3f t = pose.translation();
  std::vector<float> rt(12, 0.0f);
  rt[0] = R(0, 0);
  rt[1] = R(0, 1);
  rt[2] = R(0, 2);
  rt[3] = t.x();
  rt[4] = R(1, 0);
  rt[5] = R(1, 1);
  rt[6] = R(1, 2);
  rt[7] = t.y();
  rt[8] = R(2, 0);
  rt[9] = R(2, 1);
  rt[10] = R(2, 2);
  rt[11] = t.z();
  return rt;
}

std::vector<float> BuildPairExtrinsics3x4(const Keyframe& a, const Keyframe& b) {
  const Pose pose_a(Eigen::Quaternionf::Identity(), Eigen::Vector3f::Zero());
  const Pose pose_b = a.pose.inverse() * b.pose;
  std::vector<float> out = PoseToRt3x4RowMajor(pose_a);
  const std::vector<float> b_rt = PoseToRt3x4RowMajor(pose_b);
  out.insert(out.end(), b_rt.begin(), b_rt.end());
  return out;
}

void AppendIntArray(std::ostringstream& oss, const std::vector<int64_t>& a) {
  oss << "[";
  for (size_t i = 0; i < a.size(); ++i) {
    if (i) oss << ",";
    oss << a[i];
  }
  oss << "]";
}

void AppendFloatArray(std::ostringstream& oss, const std::vector<float>& a) {
  oss << "[";
  oss << std::setprecision(8);
  for (size_t i = 0; i < a.size(); ++i) {
    if (i) oss << ",";
    oss << a[i];
  }
  oss << "]";
}

std::string BuildInferRequestJson(const std::vector<float>& image, int w, int h) {
  std::ostringstream oss;
  oss << "{";
  oss << "\"inputs\":[";

  oss << "{";
  oss << "\"name\":\"image\",\"shape\":[1,2,3," << h << "," << w
      << "],\"datatype\":\"FP32\",\"data\":";
  AppendFloatArray(oss, image);
  oss << "}";

  oss << "],";
  oss << "\"outputs\":[{\"name\":\"depth\"},{\"name\":\"depth_conf\"},"
         "{\"name\":\"intrinsics\"},{\"name\":\"extrinsics\"}]";
  oss << "}";
  return oss.str();
}

bool ParseTritonResponse(const std::string& json, std::unordered_map<std::string, TritonTensor>* out,
                         std::string* err) {
  if (out == nullptr) return false;
  out->clear();
  try {
    const auto root = nlohmann::json::parse(json);
    if (!root.contains("outputs") || !root["outputs"].is_array()) {
      if (err) *err = "Triton response missing outputs array";
      return false;
    }
    for (const auto& node : root["outputs"]) {
      if (!node.contains("name") || !node["name"].is_string()) continue;
      const std::string name = node["name"].get<std::string>();
      if (name.empty()) continue;

      TritonTensor t;
      if (node.contains("shape") && node["shape"].is_array()) {
        for (const auto& s : node["shape"]) {
          t.shape.push_back(s.get<int64_t>());
        }
      }
      if (node.contains("data") && node["data"].is_array()) {
        t.data.reserve(node["data"].size());
        for (const auto& d : node["data"]) {
          t.data.push_back(d.get<float>());
        }
      }
      (*out)[name] = std::move(t);
    }
    return true;
  } catch (const std::exception& e) {
    if (err) *err = std::string("JSON parse error: ") + e.what();
    return false;
  }
}

bool ExtractDepthPair(const TritonTensor& t, cv::Mat* depth_a, cv::Mat* depth_b) {
  if (depth_a == nullptr || depth_b == nullptr) return false;

  if (t.shape.size() == 4 && t.shape[0] >= 1 && t.shape[2] > 0 && t.shape[3] > 0) {
    const int c = static_cast<int>(t.shape[1]);
    const int h = static_cast<int>(t.shape[2]);
    const int w = static_cast<int>(t.shape[3]);
    if (c <= 0 || h <= 0 || w <= 0) return false;
    const size_t plane = static_cast<size_t>(h) * w;
    const size_t need = (c >= 2) ? plane * 2 : plane;
    if (t.data.size() < need) return false;
    cv::Mat a(h, w, CV_32F);
    cv::Mat b(h, w, CV_32F);
    std::memcpy(a.data, t.data.data(), plane * sizeof(float));
    const size_t b_off = (c >= 2) ? plane : 0;
    std::memcpy(b.data, t.data.data() + b_off, plane * sizeof(float));
    *depth_a = a;
    *depth_b = b;
    return true;
  }

  if (t.shape.size() == 3 && t.shape[0] >= 1 && t.shape[1] > 0 && t.shape[2] > 0) {
    const int h = static_cast<int>(t.shape[1]);
    const int w = static_cast<int>(t.shape[2]);
    if (h <= 0 || w <= 0) return false;
    const size_t plane = static_cast<size_t>(h) * w;
    if (t.data.size() < plane) return false;
    cv::Mat d(h, w, CV_32F);
    std::memcpy(d.data, t.data.data(), plane * sizeof(float));
    *depth_a = d;
    *depth_b = d;
    return true;
  }

  if (t.shape.size() == 2 && t.shape[0] > 0 && t.shape[1] > 0) {
    const int h = static_cast<int>(t.shape[0]);
    const int w = static_cast<int>(t.shape[1]);
    if (h <= 0 || w <= 0) return false;
    const size_t plane = static_cast<size_t>(h) * w;
    if (t.data.size() < plane) return false;
    cv::Mat d(h, w, CV_32F);
    std::memcpy(d.data, t.data.data(), plane * sizeof(float));
    *depth_a = d;
    *depth_b = d;
    return true;
  }

  return false;
}

std::optional<Pose> ExtractDeltaPoseFromExtrinsicsTensor(const TritonTensor& t) {
  auto make_pose_3x4 = [](const float* p) -> Pose {
    Eigen::Matrix3f R;
    R << p[0], p[1], p[2], p[4], p[5], p[6], p[8], p[9], p[10];
    Eigen::Vector3f tr(p[3], p[7], p[11]);
    Eigen::Quaternionf q(R);
    q.normalize();
    return Pose(q, tr);
  };

  if (t.shape.size() == 4 && t.shape[0] >= 1 && t.shape[1] >= 2 && t.shape[2] == 3 &&
      t.shape[3] == 4 && t.data.size() >= 24) {
    const Pose a_w = make_pose_3x4(t.data.data());
    const Pose b_w = make_pose_3x4(t.data.data() + 12);
    return a_w.inverse() * b_w;
  }
  return std::nullopt;
}

bool DecodeIntrinsicsAt(const TritonTensor& t, int idx, int orig_w, int orig_h, float* fx, float* fy, float* cx,
                        float* cy) {
  if (fx == nullptr || fy == nullptr || cx == nullptr || cy == nullptr) return false;
  if (t.shape.size() != 4 || t.shape[0] != 1 || t.shape[2] != 3 || t.shape[3] != 3) return false;
  const int n = static_cast<int>(t.shape[1]);
  if (idx < 0 || idx >= n) return false;
  const size_t off = static_cast<size_t>(idx) * 9;
  if (t.data.size() < off + 9) return false;
  const float fxi = t.data[off + 0];
  const float fyi = t.data[off + 4];
  const float cxi = t.data[off + 2];
  const float cyi = t.data[off + 5];
  const float sx = static_cast<float>(orig_w) / 504.0f;
  const float sy = static_cast<float>(orig_h) / 280.0f;
  *fx = fxi * sx;
  *fy = fyi * sx;  // Follow python reference behavior.
  *cx = cxi * sx;
  *cy = cyi * sy;
  return true;
}

bool DecodeExtrinsicsAtAsPose(const TritonTensor& t, int idx, Pose* out_pose) {
  if (out_pose == nullptr) return false;
  if (t.shape.size() != 4 || t.shape[0] != 1 || t.shape[2] != 3 || t.shape[3] != 4) return false;
  const int n = static_cast<int>(t.shape[1]);
  if (idx < 0 || idx >= n) return false;
  const size_t off = static_cast<size_t>(idx) * 12;
  if (t.data.size() < off + 12) return false;
  // Model returns w2c; convert to c2w for downstream world projection usage.
  Eigen::Matrix3f R;
  R << t.data[off + 0], t.data[off + 1], t.data[off + 2], t.data[off + 4], t.data[off + 5],
      t.data[off + 6], t.data[off + 8], t.data[off + 9], t.data[off + 10];
  Eigen::Vector3f tr(t.data[off + 3], t.data[off + 7], t.data[off + 11]);
  Eigen::Quaternionf q(R);
  q.normalize();
  const Pose w2c(q, tr);
  *out_pose = w2c.inverse();
  return true;
}

std::string PoseSummary(const Pose& p) {
  const Eigen::Quaternionf q = p.unit_quaternion();
  const Eigen::Vector3f t = p.translation();
  return cv::format("q=(%.6f,%.6f,%.6f,%.6f) t=(%.6f,%.6f,%.6f) |t|=%.6f", q.x(), q.y(), q.z(),
                    q.w(), t.x(), t.y(), t.z(), t.norm());
}

}  // namespace

struct Da3OnnxRunner::Impl {
  Impl(const std::string& model_path_in, int input_width_in, int input_height_in)
      : infer_url(ResolveInferUrl(model_path_in)), input_width(input_width_in), input_height(input_height_in) {
    Init(model_path_in);
  }

  bool InferPair(const Keyframe& a, const Keyframe& b, Da3Output* output) {
    if (!ready || output == nullptr) return false;

    output->pair_label = cv::format("(kf%d,kf%d)", a.idx, b.idx);

    const std::vector<float> image = BuildImagePairNchw(a.image_bgr, b.image_bgr, input_width, input_height);
    const std::string body = BuildInferRequestJson(image, input_width, input_height);

    const auto t0 = std::chrono::steady_clock::now();
    std::string resp;
    std::string http_err;
    const std::string used_url = infer_url;
    if (!HttpPostJson(used_url, body, &resp, &http_err)) {
      output->scale_text = std::string("scale: failed (triton ") + http_err + ")";
      LOG(ERROR) << output->scale_text;
      return false;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double infer_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::unordered_map<std::string, TritonTensor> outputs;
    std::string parse_err;
    if (!ParseTritonResponse(resp, &outputs, &parse_err)) {
      output->scale_text = std::string("scale: failed (triton parse ") + parse_err + ")";
      LOG(ERROR) << output->scale_text;
      return false;
    }

    const auto it_depth = outputs.find("depth");
    if (it_depth == outputs.end()) {
      output->scale_text = "scale: failed (missing depth output)";
      return false;
    }

    const Pose input_delta = a.pose.inverse() * b.pose;
    LOG(INFO) << "[DA3] delta_input " << PoseSummary(input_delta);

    const auto it_ex = outputs.find("extrinsics");
    if (it_ex != outputs.end()) {
      auto out_delta = ExtractDeltaPoseFromExtrinsicsTensor(it_ex->second);
      if (out_delta.has_value()) {
        LOG(INFO) << "[DA3] delta_output " << PoseSummary(*out_delta);
      } else {
        LOG(INFO) << "[DA3] delta_output unavailable (could not parse extrinsics output)";
      }
    } else {
      LOG(INFO) << "[DA3] delta_output unavailable (no extrinsics output)";
    }

    cv::Mat depth_a;
    cv::Mat depth_b;
    if (!ExtractDepthPair(it_depth->second, &depth_a, &depth_b)) {
      output->scale_text = "scale: failed (invalid depth tensor)";
      return false;
    }
    cv::resize(depth_a, depth_a, a.image_bgr.size(), 0.0, 0.0, cv::INTER_NEAREST);
    cv::resize(depth_b, depth_b, b.image_bgr.size(), 0.0, 0.0, cv::INTER_NEAREST);

    output->scale_text = "scale: by-depth-consistency";
    output->kf_a_idx = a.idx;
    output->kf_b_idx = b.idx;
    output->reference_image_bgr = b.image_bgr.clone();
    output->depth_a_metric = depth_a.clone();
    output->depth_b_metric = depth_b.clone();
    output->depth_vis = ColorizeDepth(depth_b);
    output->depth_metric = depth_b.clone();
    output->pair_label = cv::format("(kf%d,kf%d) infer=%.1fms", a.idx, b.idx, infer_ms);
    output->pose = b.pose;
    output->fx = b.fx;
    output->fy = b.fy;
    output->cx = b.cx;
    output->cy = b.cy;
    const auto it_intri = outputs.find("intrinsics");
    if (it_intri != outputs.end()) {
      float fx = 0.0f, fy = 0.0f, cx = 0.0f, cy = 0.0f;
      if (DecodeIntrinsicsAt(it_intri->second, 1, b.image_bgr.cols, b.image_bgr.rows, &fx, &fy, &cx, &cy)) {
        output->fx = fx;
        output->fy = fy;
        output->cx = cx;
        output->cy = cy;
      }
    }
    if (it_ex != outputs.end()) {
      Pose p;
      if (DecodeExtrinsicsAtAsPose(it_ex->second, 1, &p)) {
        output->pose = p;
      }
    }
    return !output->depth_vis.empty();
  }

  void Init(const std::string& model_path_in) {
    if (infer_url.empty()) {
      error_msg = "Invalid Triton URL. Use --da3_model as Triton URL or infer endpoint.";
      return;
    }
    ready = true;
    LOG(INFO) << "[DA3] Triton infer endpoint: " << infer_url;
    LOG(INFO) << "[DA3] Hint: pass --da3_model=http://<host>:8000 or full /v2/models/<name>/infer URL";
  }

  std::string infer_url;
  int input_width = 392;
  int input_height = 224;
  bool ready = false;
  std::string error_msg;
};

Da3OnnxRunner::Da3OnnxRunner(const std::string& model_path, int input_width, int input_height)
    : impl_(std::make_unique<Impl>(model_path, input_width, input_height)) {}

Da3OnnxRunner::~Da3OnnxRunner() = default;

bool Da3OnnxRunner::IsReady() const { return impl_ && impl_->ready; }

const std::string& Da3OnnxRunner::ErrorMessage() const {
  static const std::string kEmpty;
  if (!impl_) return kEmpty;
  return impl_->error_msg;
}

bool Da3OnnxRunner::InferPair(const Keyframe& a, const Keyframe& b, Da3Output* output) {
  return impl_ && impl_->InferPair(a, b, output);
}

Da3Worker::Da3Worker(std::unique_ptr<Da3OnnxRunner> runner)
    : runner_(std::move(runner)), worker_(&Da3Worker::ThreadMain, this) {
  std::lock_guard<std::mutex> lock(mu_);
  last_status_ = "DA3: waiting for first keyframe pair";
}

Da3Worker::~Da3Worker() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    jobs_.clear();
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

bool Da3Worker::IsReady() const { return runner_ != nullptr && runner_->IsReady(); }

std::string Da3Worker::ErrorMessage() const {
  if (!runner_) return "DA3 runner missing";
  return runner_->ErrorMessage();
}

void Da3Worker::Submit(const Keyframe& a, const Keyframe& b) {
  std::lock_guard<std::mutex> lock(mu_);
  jobs_.clear();
  jobs_.push_back({a, b});
  last_status_ = cv::format("DA3: queued (kf%d,kf%d)", a.idx, b.idx);
  cv_.notify_one();
}

std::optional<Da3Output> Da3Worker::GetLatestOutput() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!latest_output_.has_value()) return std::nullopt;
  Da3Output out;
  out.kf_a_idx = latest_output_->kf_a_idx;
  out.kf_b_idx = latest_output_->kf_b_idx;
  out.scale_text = latest_output_->scale_text;
  out.pair_label = latest_output_->pair_label;
  out.reference_image_bgr = latest_output_->reference_image_bgr.clone();
  out.depth_a_metric = latest_output_->depth_a_metric.clone();
  out.depth_b_metric = latest_output_->depth_b_metric.clone();
  out.depth_vis = latest_output_->depth_vis.clone();
  out.depth_metric = latest_output_->depth_metric.clone();
  out.pose = latest_output_->pose;
  out.fx = latest_output_->fx;
  out.fy = latest_output_->fy;
  out.cx = latest_output_->cx;
  out.cy = latest_output_->cy;
  return out;
}

std::string Da3Worker::GetLatestStatus() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_status_;
}

void Da3Worker::ThreadMain() {
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [&]() { return stop_ || !jobs_.empty(); });
      if (stop_) return;
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

}  // namespace da3client
