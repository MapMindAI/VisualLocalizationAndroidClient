#include "mapping/common/ros2_utils.h"
#include "mapping/voxblox/voxblox_processor.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <Eigen/Core>
#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <message_filters/subscriber.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/synchronizer.h>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

DEFINE_string(node_name, "voxblox_ros_node", "ROS2 node name.");
DEFINE_string(topic_depth, "/vlp/depth", "Depth image topic.");
DEFINE_string(topic_pose, "/vlp/pose", "VIO pose topic.");
DEFINE_string(topic_esdf, "/vlp/esdf_cloud", "ESDF cloud topic.");
DEFINE_string(topic_depth_cloud, "/vlp/depth_cloud", "Depth cloud topic in world frame.");
DEFINE_string(frame_id, "map", "Output cloud frame id.");
DEFINE_double(esdf_publish_hz, 2.0, "ESDF publish frequency.");
DEFINE_double(voxel_size_m, 0.2, "Voxblox voxel size.");
DEFINE_double(max_depth_m, 12.0, "Max depth for integration.");
DEFINE_double(min_depth_m, 0.2, "Max depth for integration.");
DEFINE_int32(depth_stride, 8, "Depth sampling stride.");
DEFINE_double(depth_fx, 313.94085693359375, "Depth intrinsics fx.");
DEFINE_double(depth_fy, 313.94085693359375, "Depth intrinsics fy.");
DEFINE_double(depth_cx, 269.742431640625, "Depth intrinsics cx.");
DEFINE_double(depth_cy, 316.34063720703125, "Depth intrinsics cy.");
DEFINE_int32(sync_queue_size, 50, "ApproximateTime sync queue size for pose+depth.");

namespace {

using Pose = Sophus::SE3f;
using ApproxPolicy = message_filters::sync_policies::ApproximateTime<
    geometry_msgs::msg::PoseStamped, sensor_msgs::msg::Image>;

class VoxbloxRosNode final : public rclcpp::Node {
 public:
  VoxbloxRosNode()
      : rclcpp::Node(FLAGS_node_name),
        voxblox_(std::make_unique<mapping::VoxbloxProcessor>(MakeCfg())) {
    pose_sub_.subscribe(this, FLAGS_topic_pose);
    depth_sub_.subscribe(this, FLAGS_topic_depth);
    sync_ = std::make_shared<message_filters::Synchronizer<ApproxPolicy>>(
        ApproxPolicy(std::max(1, FLAGS_sync_queue_size)), pose_sub_, depth_sub_);
    sync_->registerCallback(
        std::bind(&VoxbloxRosNode::OnSyncedPoseDepth, this, std::placeholders::_1,
                  std::placeholders::_2));
    pub_esdf_ = create_publisher<sensor_msgs::msg::PointCloud2>(FLAGS_topic_esdf, 5);
    pub_depth_cloud_ =
        create_publisher<sensor_msgs::msg::PointCloud2>(FLAGS_topic_depth_cloud, 10);
    timer_ = create_wall_timer(
        std::chrono::duration<double>(1.0 / std::max(1e-3, FLAGS_esdf_publish_hz)),
        std::bind(&VoxbloxRosNode::OnTimer, this));
  }

 private:
  static mapping::VoxbloxProcessor::Config MakeCfg() {
    mapping::VoxbloxProcessor::Config cfg(FLAGS_voxel_size_m, FLAGS_max_depth_m);
    cfg.pixel_step = std::max(1, FLAGS_depth_stride);
    return cfg;
  }

  void OnSyncedPoseDepth(const geometry_msgs::msg::PoseStamped::ConstSharedPtr& pose_msg,
                         const sensor_msgs::msg::Image::ConstSharedPtr& depth_msg) {
    const auto& p = pose_msg->pose.position;
    const auto& q = pose_msg->pose.orientation;
    const Pose pose(Eigen::Quaternionf(static_cast<float>(q.w), static_cast<float>(q.x),
                                       static_cast<float>(q.y), static_cast<float>(q.z)),
                    Eigen::Vector3f(static_cast<float>(p.x), static_cast<float>(p.y),
                                    static_cast<float>(p.z)));
    cv::Mat depth_m;
    if (!DecodeDepth(*depth_msg, &depth_m)) return;

    const auto points = BuildDepthPointsCamera(depth_m);
    if (!points.empty()) {
      voxblox_->IntegratePointCloud(points, pose);
      std::vector<Eigen::Vector3f> cloud_w;
      cloud_w.reserve(points.size());
      const Eigen::Matrix3f R = pose.rotationMatrix();
      const Eigen::Vector3f t = pose.translation();
      for (const auto& p : points) {
        cloud_w.push_back(R * p + t);
      }
      pub_depth_cloud_->publish(mapping::ros2::MakeXYZCloudMsg(
          cloud_w, FLAGS_frame_id, rclcpp::Time(depth_msg->header.stamp)));
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
    pub_esdf_->publish(mapping::ros2::MakeXYZRGBCloudMsg(cloud, FLAGS_frame_id, now()));
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
    const float min_d = static_cast<float>(FLAGS_min_depth_m);
    pts.reserve(static_cast<size_t>((depth_m.rows / s + 1) * (depth_m.cols / s + 1)));

    // the depth corresponding to the left eye, so the depth of the first few col might be wrong
    // we skip from the beginning cols
    int start_col = depth_m.cols * 0.05;
    for (int v = 0; v < depth_m.rows; v += s) {
      const float* row = depth_m.ptr<float>(v);
      for (int u = start_col; u < depth_m.cols; u += s) {
        const float z = row[u];
        if (!(z > min_d) || !std::isfinite(z) || z > max_d) continue;
        const float x = (static_cast<float>(u) - cx) * z / fx;
        const float y = (static_cast<float>(v) - cy) * z / fy;
        pts.emplace_back(x, y, z);
      }
    }
    return pts;
  }

  std::unique_ptr<mapping::VoxbloxProcessor> voxblox_;
  message_filters::Subscriber<geometry_msgs::msg::PoseStamped> pose_sub_;
  message_filters::Subscriber<sensor_msgs::msg::Image> depth_sub_;
  std::shared_ptr<message_filters::Synchronizer<ApproxPolicy>> sync_;
  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pub_esdf_;
  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pub_depth_cloud_;
  rclcpp::TimerBase::SharedPtr timer_;
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
