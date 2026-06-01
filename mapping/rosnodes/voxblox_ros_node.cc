#include "mapping/voxblox/voxblox_processor.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <Eigen/Core>
#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/msg/point_field.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

DEFINE_string(node_name, "voxblox_ros_node", "ROS2 node name.");
DEFINE_string(topic_depth, "/vlp/depth", "Depth image topic.");
DEFINE_string(topic_pose, "/vlp/pose", "VIO pose topic.");
DEFINE_string(topic_esdf, "/vlp/esdf_cloud", "ESDF cloud topic.");
DEFINE_string(frame_id, "map", "Output cloud frame id.");
DEFINE_double(esdf_publish_hz, 2.0, "ESDF publish frequency.");
DEFINE_double(voxel_size_m, 0.2, "Voxblox voxel size.");
DEFINE_double(max_depth_m, 8.0, "Max depth for integration.");
DEFINE_int32(depth_stride, 4, "Depth sampling stride.");
DEFINE_double(depth_fx, 313.94085693359375, "Depth intrinsics fx.");
DEFINE_double(depth_fy, 313.94085693359375, "Depth intrinsics fy.");
DEFINE_double(depth_cx, 269.742431640625, "Depth intrinsics cx.");
DEFINE_double(depth_cy, 316.34063720703125, "Depth intrinsics cy.");

namespace {

using Pose = Sophus::SE3f;

sensor_msgs::msg::PointCloud2 MakeXYZRGBCloudMsg(
    const std::vector<std::array<float, 6>>& points, const std::string& frame_id,
    const rclcpp::Time& stamp) {
  sensor_msgs::msg::PointCloud2 msg;
  msg.header.frame_id = frame_id;
  msg.header.stamp = stamp;
  msg.height = 1;
  msg.width = static_cast<uint32_t>(points.size());
  msg.is_bigendian = false;
  msg.is_dense = false;
  msg.point_step = 16;
  msg.row_step = msg.point_step * msg.width;
  msg.fields.resize(4);
  msg.fields[0].name = "x";
  msg.fields[0].offset = 0;
  msg.fields[0].datatype = sensor_msgs::msg::PointField::FLOAT32;
  msg.fields[0].count = 1;
  msg.fields[1].name = "y";
  msg.fields[1].offset = 4;
  msg.fields[1].datatype = sensor_msgs::msg::PointField::FLOAT32;
  msg.fields[1].count = 1;
  msg.fields[2].name = "z";
  msg.fields[2].offset = 8;
  msg.fields[2].datatype = sensor_msgs::msg::PointField::FLOAT32;
  msg.fields[2].count = 1;
  msg.fields[3].name = "rgb";
  msg.fields[3].offset = 12;
  msg.fields[3].datatype = sensor_msgs::msg::PointField::FLOAT32;
  msg.fields[3].count = 1;
  msg.data.resize(msg.row_step);
  for (size_t i = 0; i < points.size(); ++i) {
    uint8_t* p = msg.data.data() + i * msg.point_step;
    std::memcpy(p + 0, &points[i][0], sizeof(float));
    std::memcpy(p + 4, &points[i][1], sizeof(float));
    std::memcpy(p + 8, &points[i][2], sizeof(float));
    const uint8_t r = static_cast<uint8_t>(std::clamp(points[i][3], 0.0f, 255.0f));
    const uint8_t g = static_cast<uint8_t>(std::clamp(points[i][4], 0.0f, 255.0f));
    const uint8_t b = static_cast<uint8_t>(std::clamp(points[i][5], 0.0f, 255.0f));
    const uint32_t rgb_u32 =
        (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    float rgb_f32 = 0.0f;
    std::memcpy(&rgb_f32, &rgb_u32, sizeof(float));
    std::memcpy(p + 12, &rgb_f32, sizeof(float));
  }
  return msg;
}

class VoxbloxRosNode final : public rclcpp::Node {
 public:
  VoxbloxRosNode()
      : rclcpp::Node(FLAGS_node_name),
        voxblox_(std::make_unique<mapping::VoxbloxProcessor>(MakeCfg())) {
    sub_pose_ = create_subscription<geometry_msgs::msg::PoseStamped>(
        FLAGS_topic_pose, 50, std::bind(&VoxbloxRosNode::OnPose, this, std::placeholders::_1));
    sub_depth_ = create_subscription<sensor_msgs::msg::Image>(
        FLAGS_topic_depth, 20, std::bind(&VoxbloxRosNode::OnDepth, this, std::placeholders::_1));
    pub_esdf_ = create_publisher<sensor_msgs::msg::PointCloud2>(FLAGS_topic_esdf, 5);
    timer_ = create_wall_timer(
        std::chrono::duration<double>(1.0 / std::max(1e-3, FLAGS_esdf_publish_hz)),
        std::bind(&VoxbloxRosNode::OnTimer, this));
  }

 private:
  static mapping::VoxbloxProcessor::Config MakeCfg() {
    mapping::VoxbloxProcessor::Config cfg(static_cast<float>(FLAGS_voxel_size_m));
    cfg.max_depth_m = static_cast<float>(FLAGS_max_depth_m);
    cfg.max_ray_length_m = static_cast<float>(FLAGS_max_depth_m);
    cfg.pixel_step = std::max(1, FLAGS_depth_stride);
    return cfg;
  }

  void OnPose(const geometry_msgs::msg::PoseStamped::SharedPtr msg) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto& p = msg->pose.position;
    const auto& q = msg->pose.orientation;
    latest_pose_ = Pose(Eigen::Quaternionf(static_cast<float>(q.w), static_cast<float>(q.x),
                                           static_cast<float>(q.y), static_cast<float>(q.z)),
                        Eigen::Vector3f(static_cast<float>(p.x), static_cast<float>(p.y),
                                        static_cast<float>(p.z)));
    has_pose_ = true;
  }

  void OnDepth(const sensor_msgs::msg::Image::SharedPtr msg) {
    Pose pose;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!has_pose_) return;
      pose = latest_pose_;
    }
    cv::Mat depth_m;
    if (!DecodeDepth(*msg, &depth_m)) return;

    const auto points = BuildDepthPointsCamera(depth_m);
    if (!points.empty()) {
      voxblox_->IntegratePointCloud(points, pose);
    }
  }

  void OnTimer() {
    std::vector<mapping::VoxbloxProcessor::VizPoint> esdf;
    voxblox_->GetEsdfVisualization(&esdf);
    std::vector<std::array<float, 6>> cloud;
    cloud.reserve(esdf.size());
    for (const auto& p : esdf) {
      cloud.push_back({p.x, p.y, p.z, p.r, p.g, p.b});
    }
    pub_esdf_->publish(MakeXYZRGBCloudMsg(cloud, FLAGS_frame_id, now()));
  }

  bool DecodeDepth(const sensor_msgs::msg::Image& msg, cv::Mat* out) const {
    if (msg.data.empty() || msg.height == 0 || msg.width == 0) return false;
    if (msg.encoding == "32FC1") {
      cv::Mat m(static_cast<int>(msg.height), static_cast<int>(msg.width), CV_32FC1,
                const_cast<uint8_t*>(msg.data.data()), static_cast<size_t>(msg.step));
      *out = m.clone();
      return true;
    }
    if (msg.encoding == "16UC1" || msg.encoding == "mono16") {
      cv::Mat m16(static_cast<int>(msg.height), static_cast<int>(msg.width), CV_16UC1,
                  const_cast<uint8_t*>(msg.data.data()), static_cast<size_t>(msg.step));
      m16.convertTo(*out, CV_32FC1, 1.0 / 1000.0);
      return true;
    }
    return false;
  }

  std::vector<Eigen::Vector3f> BuildDepthPointsCamera(const cv::Mat& depth_m) const {
    std::vector<Eigen::Vector3f> pts;
    const int s = std::max(1, FLAGS_depth_stride);
    const float fx = static_cast<float>(FLAGS_depth_fx);
    const float fy = static_cast<float>(FLAGS_depth_fy);
    const float cx = static_cast<float>(FLAGS_depth_cx);
    const float cy = static_cast<float>(FLAGS_depth_cy);
    const float max_d = static_cast<float>(FLAGS_max_depth_m);
    pts.reserve(static_cast<size_t>((depth_m.rows / s + 1) * (depth_m.cols / s + 1)));
    for (int v = 0; v < depth_m.rows; v += s) {
      const float* row = depth_m.ptr<float>(v);
      for (int u = 0; u < depth_m.cols; u += s) {
        const float z = row[u];
        if (!(z > 0.01f) || !std::isfinite(z) || z > max_d) continue;
        const float x = (static_cast<float>(u) - cx) * z / fx;
        const float y = (static_cast<float>(v) - cy) * z / fy;
        pts.emplace_back(x, y, z);
      }
    }
    return pts;
  }

  std::unique_ptr<mapping::VoxbloxProcessor> voxblox_;
  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr sub_depth_;
  rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr sub_pose_;
  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pub_esdf_;
  rclcpp::TimerBase::SharedPtr timer_;

  std::mutex mu_;
  Pose latest_pose_ = Pose();
  bool has_pose_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<VoxbloxRosNode>());
  rclcpp::shutdown();
  google::ShutdownGoogleLogging();
  return 0;
}
