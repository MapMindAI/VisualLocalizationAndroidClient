#pragma once

#include <Eigen/Core>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>

#include <array>
#include <string>
#include <vector>

namespace mapping::ros2 {

struct ColoredPoint {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  uint8_t r = 255;
  uint8_t g = 255;
  uint8_t b = 255;
};

sensor_msgs::msg::PointCloud2 MakeXYZRGBCloudMsg(
    const std::vector<std::array<float, 6>>& points, const std::string& frame_id,
    const rclcpp::Time& stamp);

sensor_msgs::msg::PointCloud2 MakeXYZCloudMsg(const std::vector<Eigen::Vector3f>& points,
                                              const std::string& frame_id,
                                              const rclcpp::Time& stamp);

bool FindFieldOffset(const sensor_msgs::msg::PointCloud2& msg, const std::string& name, int* offset);

std::vector<ColoredPoint> DecodePointCloud2(const sensor_msgs::msg::PointCloud2& msg);

}  // namespace mapping::ros2

