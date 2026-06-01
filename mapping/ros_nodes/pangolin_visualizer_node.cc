#include "mapping/common/ros2_utils.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <pangolin/pangolin.h>

#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>

DEFINE_string(node_name, "pangolin_visualizer_node", "ROS2 node name.");
DEFINE_string(topic_rgb_compressed, "/vlp/rgb/compressed", "Compressed RGB image topic.");
DEFINE_string(topic_depth, "/vlp/depth", "Depth image topic.");
DEFINE_string(topic_pose, "/vlp/pose", "Pose topic.");
DEFINE_string(topic_esdf, "/vlp/esdf_cloud", "ESDF cloud topic.");
DEFINE_string(topic_depth_cloud, "/vlp/depth_cloud", "Depth cloud topic (world frame).");
DEFINE_int32(window_width, 1600, "Viewer width.");
DEFINE_int32(window_height, 900, "Viewer height.");
DEFINE_int32(max_traj_points, 5000, "Max trajectory points.");
DEFINE_bool(esdf_color_by_height, false, "Color ESDF points by height z-axis (Jet).");
DEFINE_double(esdf_height_min_m, -2.0, "Height min (m) used for ESDF Jet normalization.");
DEFINE_double(esdf_height_max_m, 2.0, "Height max (m) used for ESDF Jet normalization.");

namespace {

Eigen::Vector3f JetRgb(float t) {
  t = std::max(0.0f, std::min(1.0f, t));
  const float r = std::max(0.0f, std::min(1.0f, 1.5f - std::fabs(4.0f * t - 3.0f)));
  const float g = std::max(0.0f, std::min(1.0f, 1.5f - std::fabs(4.0f * t - 2.0f)));
  const float b = std::max(0.0f, std::min(1.0f, 1.5f - std::fabs(4.0f * t - 1.0f)));
  return Eigen::Vector3f(r, g, b);
}

class PangolinVisualizerNode final : public rclcpp::Node {
 public:
  PangolinVisualizerNode() : rclcpp::Node(FLAGS_node_name) {
    sub_rgb_compressed_ = create_subscription<sensor_msgs::msg::CompressedImage>(
        FLAGS_topic_rgb_compressed, 10,
        std::bind(&PangolinVisualizerNode::OnRgbCompressed, this, std::placeholders::_1));
    sub_depth_ = create_subscription<sensor_msgs::msg::Image>(
        FLAGS_topic_depth, 10, std::bind(&PangolinVisualizerNode::OnDepth, this, std::placeholders::_1));
    sub_pose_ = create_subscription<geometry_msgs::msg::PoseStamped>(
        FLAGS_topic_pose, 50, std::bind(&PangolinVisualizerNode::OnPose, this, std::placeholders::_1));
    sub_esdf_ = create_subscription<sensor_msgs::msg::PointCloud2>(
        FLAGS_topic_esdf, 5, std::bind(&PangolinVisualizerNode::OnEsdf, this, std::placeholders::_1));
    sub_depth_cloud_ = create_subscription<sensor_msgs::msg::PointCloud2>(
        FLAGS_topic_depth_cloud, 10,
        std::bind(&PangolinVisualizerNode::OnDepthCloud, this, std::placeholders::_1));
  }

  void RunViewer() {
    pangolin::CreateWindowAndBind("Pangolin Visualizer Node", FLAGS_window_width, FLAGS_window_height);
    glEnable(GL_DEPTH_TEST);
    pangolin::CreatePanel("menu").SetBounds(0.0, 1.0, 0.0, pangolin::Attach::Pix(220));
    pangolin::Var<bool> ui_show_depth_cloud("menu.Show DepthCloud", true, true);
    pangolin::Var<double> ui_depth_cloud_size("menu.DepthCloud Size", 2.0, 1.0, 10.0, true);
    pangolin::Var<double> ui_esdf_size("menu.ESDF Size", 3.0, 1.0, 10.0, true);
    pangolin::Var<double> ui_traj_width("menu.Traj Width", 2.0, 1.0, 8.0, true);
    pangolin::Var<double> ui_cam_axis("menu.Cam Axis", 0.35, 0.05, 2.0, true);
    pangolin::Var<bool> ui_follow_camera("menu.Follow Camera", false, true);
    pangolin::Var<bool> ui_esdf_color_height("menu.ESDF Color Height", FLAGS_esdf_color_by_height, true);
    pangolin::Var<double> ui_esdf_hmin("menu.ESDF H min", FLAGS_esdf_height_min_m, -20.0, 20.0, false);
    pangolin::Var<double> ui_esdf_hmax("menu.ESDF H max", FLAGS_esdf_height_max_m, -20.0, 20.0, false);

    pangolin::OpenGlRenderState s_cam(
        pangolin::ProjectionMatrix(1280, 720, 700, 700, 640, 360, 0.1, 2000),
        pangolin::ModelViewLookAt(0, -3, -6, 0, 0, 0, pangolin::AxisY));
    pangolin::View& d_3d = pangolin::CreateDisplay()
                               .SetBounds(0.32, 1.0, pangolin::Attach::Pix(220), 1.0,
                                          -1280.0f / 720.0f)
                               .SetHandler(new pangolin::Handler3D(s_cam));
    pangolin::View& d_rgb =
        pangolin::Display("rgb").SetBounds(0.0, 0.32, pangolin::Attach::Pix(220), 0.6, -1.0);
    pangolin::View& d_depth =
        pangolin::Display("depth").SetBounds(0.0, 0.32, 0.6, 1.0, -1.0);
    pangolin::GlTexture tex_rgb(640, 480, GL_RGB, false, 0, GL_RGB, GL_UNSIGNED_BYTE);
    pangolin::GlTexture tex_depth(640, 480, GL_RGB, false, 0, GL_RGB, GL_UNSIGNED_BYTE);

    while (!pangolin::ShouldQuit() && rclcpp::ok()) {
      cv::Mat rgb, depth_vis;
      std::vector<mapping::ros2::ColoredPoint> esdf;
      std::vector<mapping::ros2::ColoredPoint> depth_cloud;
      std::deque<Eigen::Vector3f> traj;
      Eigen::Vector3f t = Eigen::Vector3f::Zero();
      Eigen::Quaternionf q = Eigen::Quaternionf::Identity();
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (!latest_rgb_.empty()) rgb = latest_rgb_.clone();
        if (!latest_depth_vis_.empty()) depth_vis = latest_depth_vis_.clone();
        esdf = latest_esdf_;
        depth_cloud = latest_depth_cloud_;
        traj = traj_;
        t = cam_t_;
        q = cam_q_;
      }

      if (ui_follow_camera && !traj.empty()) {
        pangolin::OpenGlMatrix p_mat;
        Eigen::Map<Eigen::Matrix<double, 4, 4>> p_matrix(p_mat.m);
        p_matrix = Eigen::Matrix<double, 4, 4>::Identity();
        p_matrix.block(0, 3, 3, 1) = traj.back().cast<double>();
        s_cam.Follow(p_mat);
      }

      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      d_3d.Activate(s_cam);
      pangolin::glDrawAxis(1.0);
      if (ui_show_depth_cloud) {
        glPointSize(static_cast<float>(ui_depth_cloud_size.Get()));
        glBegin(GL_POINTS);
        for (const auto& p : depth_cloud) {
          glColor3ub(180, 220, 255);
          glVertex3f(p.x, p.y, p.z);
        }
        glEnd();
      }

      glPointSize(static_cast<float>(ui_esdf_size.Get()));
      glBegin(GL_POINTS);
      for (const auto& p : esdf) {
        if (ui_esdf_color_height) {
          const float hmin = static_cast<float>(ui_esdf_hmin.Get());
          const float hmax = static_cast<float>(ui_esdf_hmax.Get());
          const float denom = std::max(1e-3f, hmax - hmin);
          const float h01 = (p.z - hmin) / denom;
          const Eigen::Vector3f c = JetRgb(h01);
          glColor3f(c.x(), c.y(), c.z());
        } else {
          glColor3ub(p.r, p.g, p.b);
        }
        glVertex3f(p.x, p.y, p.z);
      }
      glEnd();

      if (traj.size() > 1) {
        glLineWidth(static_cast<float>(ui_traj_width.Get()));
        glColor3ub(100, 255, 100);
        glBegin(GL_LINE_STRIP);
        for (const auto& p : traj) glVertex3f(p.x(), p.y(), p.z());
        glEnd();
      }

      const Eigen::Matrix3f R = q.normalized().toRotationMatrix();
      const float axis_len = static_cast<float>(ui_cam_axis.Get());
      const Eigen::Vector3f ex = t + R.col(0) * axis_len;
      const Eigen::Vector3f ey = t + R.col(1) * axis_len;
      const Eigen::Vector3f ez = t + R.col(2) * axis_len;
      glLineWidth(3.0f);
      glBegin(GL_LINES);
      glColor3ub(255, 0, 0); glVertex3f(t.x(), t.y(), t.z()); glVertex3f(ex.x(), ex.y(), ex.z());
      glColor3ub(0, 255, 0); glVertex3f(t.x(), t.y(), t.z()); glVertex3f(ey.x(), ey.y(), ey.z());
      glColor3ub(0, 0, 255); glVertex3f(t.x(), t.y(), t.z()); glVertex3f(ez.x(), ez.y(), ez.z());
      glEnd();

      if (!rgb.empty()) {
        cv::Mat rgb_rot;
        cv::rotate(rgb, rgb_rot, cv::ROTATE_90_COUNTERCLOCKWISE);
        d_rgb.SetAspect(static_cast<double>(rgb_rot.cols) / static_cast<double>(rgb_rot.rows));
        if (tex_rgb.width != rgb_rot.cols || tex_rgb.height != rgb_rot.rows) {
          tex_rgb.Reinitialise(rgb_rot.cols, rgb_rot.rows, GL_RGB, false, 0, GL_BGR, GL_UNSIGNED_BYTE);
        }
        tex_rgb.Upload(rgb_rot.data, GL_BGR, GL_UNSIGNED_BYTE);
        d_rgb.Activate();
        glColor3f(1.0, 1.0, 1.0);
        tex_rgb.RenderToViewportFlipY();
      }
      if (!depth_vis.empty()) {
        cv::Mat depth_rot;
        cv::rotate(depth_vis, depth_rot, cv::ROTATE_90_COUNTERCLOCKWISE);
        d_depth.SetAspect(static_cast<double>(depth_rot.cols) / static_cast<double>(depth_rot.rows));
        if (tex_depth.width != depth_rot.cols || tex_depth.height != depth_rot.rows) {
          tex_depth.Reinitialise(depth_rot.cols, depth_rot.rows, GL_RGB, false, 0, GL_BGR,
                                 GL_UNSIGNED_BYTE);
        }
        tex_depth.Upload(depth_rot.data, GL_BGR, GL_UNSIGNED_BYTE);
        d_depth.Activate();
        glColor3f(1.0, 1.0, 1.0);
        tex_depth.RenderToViewportFlipY();
      }
      pangolin::FinishFrame();
    }
  }

 private:
  void OnRgbCompressed(const sensor_msgs::msg::CompressedImage::SharedPtr msg) {
    if (!msg || msg->data.empty()) return;
    cv::Mat encoded(1, static_cast<int>(msg->data.size()), CV_8UC1,
                    const_cast<uint8_t*>(msg->data.data()));
    cv::Mat bgr = cv::imdecode(encoded, cv::IMREAD_COLOR);
    if (bgr.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    latest_rgb_ = bgr;
  }

  void OnDepth(const sensor_msgs::msg::Image::SharedPtr msg) {
    if (!msg || msg->data.empty()) return;
    cv::Mat depth_m;
    if (msg->encoding == "32FC1") {
      cv::Mat m(static_cast<int>(msg->height), static_cast<int>(msg->width), CV_32FC1,
                const_cast<uint8_t*>(msg->data.data()), static_cast<size_t>(msg->step));
      depth_m = m.clone();
    } else if (msg->encoding == "16UC1" || msg->encoding == "mono16") {
      cv::Mat m16(static_cast<int>(msg->height), static_cast<int>(msg->width), CV_16UC1,
                  const_cast<uint8_t*>(msg->data.data()), static_cast<size_t>(msg->step));
      m16.convertTo(depth_m, CV_32FC1, 1.0 / 1000.0);
    } else {
      LOG(WARNING) << msg->encoding;
      return;
    }
    double maxv = 0.0;
    cv::minMaxLoc(depth_m, nullptr, &maxv);
    if (maxv <= 1e-6) maxv = 1.0;
    cv::Mat d8, color;
    depth_m.convertTo(d8, CV_8U, 255.0 / maxv);
    cv::applyColorMap(d8, color, cv::COLORMAP_TURBO);
    std::lock_guard<std::mutex> lk(mu_);
    latest_depth_vis_ = color;
  }

  void OnPose(const geometry_msgs::msg::PoseStamped::SharedPtr msg) {
    if (!msg) return;
    const auto& p = msg->pose.position;
    const auto& q = msg->pose.orientation;
    std::lock_guard<std::mutex> lk(mu_);
    cam_t_ = Eigen::Vector3f(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
    cam_q_ = Eigen::Quaternionf(static_cast<float>(q.w), static_cast<float>(q.x), static_cast<float>(q.y),
                                static_cast<float>(q.z));
    traj_.push_back(cam_t_);
    while (static_cast<int>(traj_.size()) > FLAGS_max_traj_points) traj_.pop_front();
  }

  void OnEsdf(const sensor_msgs::msg::PointCloud2::SharedPtr msg) {
    if (!msg) return;
    auto esdf = mapping::ros2::DecodePointCloud2(*msg);
    std::lock_guard<std::mutex> lk(mu_);
    latest_esdf_ = std::move(esdf);
  }

  void OnDepthCloud(const sensor_msgs::msg::PointCloud2::SharedPtr msg) {
    if (!msg) return;
    auto pts = mapping::ros2::DecodePointCloud2(*msg);
    std::lock_guard<std::mutex> lk(mu_);
    latest_depth_cloud_ = std::move(pts);
  }

  rclcpp::Subscription<sensor_msgs::msg::CompressedImage>::SharedPtr sub_rgb_compressed_;
  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr sub_depth_;
  rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr sub_pose_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr sub_esdf_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr sub_depth_cloud_;

  std::mutex mu_;
  cv::Mat latest_rgb_;
  cv::Mat latest_depth_vis_;
  std::vector<mapping::ros2::ColoredPoint> latest_esdf_;
  std::vector<mapping::ros2::ColoredPoint> latest_depth_cloud_;
  std::deque<Eigen::Vector3f> traj_;
  Eigen::Vector3f cam_t_ = Eigen::Vector3f::Zero();
  Eigen::Quaternionf cam_q_ = Eigen::Quaternionf::Identity();
};

}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  rclcpp::init(argc, argv);
  auto node = std::make_shared<PangolinVisualizerNode>();
  std::thread spin_thread([&]() { rclcpp::spin(node); });
  node->RunViewer();
  rclcpp::shutdown();
  if (spin_thread.joinable()) spin_thread.join();
  google::ShutdownGoogleLogging();
  return 0;
}
