#include "mapping/common/ros2_utils.h"

#include <sensor_msgs/msg/point_field.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mapping::ros2 {

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

sensor_msgs::msg::PointCloud2 MakeXYZCloudMsg(const std::vector<Eigen::Vector3f>& points,
                                              const std::string& frame_id,
                                              const rclcpp::Time& stamp) {
  sensor_msgs::msg::PointCloud2 msg;
  msg.header.frame_id = frame_id;
  msg.header.stamp = stamp;
  msg.height = 1;
  msg.width = static_cast<uint32_t>(points.size());
  msg.is_bigendian = false;
  msg.is_dense = false;
  msg.point_step = 12;
  msg.row_step = msg.point_step * msg.width;
  msg.fields.resize(3);
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
  msg.data.resize(msg.row_step);
  for (size_t i = 0; i < points.size(); ++i) {
    uint8_t* p = msg.data.data() + i * msg.point_step;
    std::memcpy(p + 0, &points[i].x(), sizeof(float));
    std::memcpy(p + 4, &points[i].y(), sizeof(float));
    std::memcpy(p + 8, &points[i].z(), sizeof(float));
  }
  return msg;
}

bool FindFieldOffset(const sensor_msgs::msg::PointCloud2& msg, const std::string& name, int* offset) {
  for (const auto& f : msg.fields) {
    if (f.name == name) {
      *offset = static_cast<int>(f.offset);
      return true;
    }
  }
  return false;
}

std::vector<ColoredPoint> DecodePointCloud2(const sensor_msgs::msg::PointCloud2& msg,
                                            const Eigen::Vector3f* origin) {
  std::vector<ColoredPoint> out;
  if (msg.point_step < 12 || msg.width == 0 || msg.data.empty()) return out;
  int off_x = -1, off_y = -1, off_z = -1, off_rgb = -1;
  if (!FindFieldOffset(msg, "x", &off_x) || !FindFieldOffset(msg, "y", &off_y) ||
      !FindFieldOffset(msg, "z", &off_z)) {
    return out;
  }
  FindFieldOffset(msg, "rgb", &off_rgb);
  const size_t n = static_cast<size_t>(msg.width) * static_cast<size_t>(msg.height);
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* p = msg.data.data() + i * msg.point_step;
    ColoredPoint cp;
    std::memcpy(&cp.x, p + off_x, sizeof(float));
    std::memcpy(&cp.y, p + off_y, sizeof(float));
    std::memcpy(&cp.z, p + off_z, sizeof(float));
    if (origin != nullptr) {
      cp.x -= origin->x();
      cp.y -= origin->y();
      cp.z -= origin->z();
    }
    if (!std::isfinite(cp.x) || !std::isfinite(cp.y) || !std::isfinite(cp.z)) continue;
    if (off_rgb >= 0 && off_rgb + 4 <= static_cast<int>(msg.point_step)) {
      uint32_t rgb_u32 = 0;
      std::memcpy(&rgb_u32, p + off_rgb, sizeof(uint32_t));
      cp.r = static_cast<uint8_t>((rgb_u32 >> 16) & 0xFF);
      cp.g = static_cast<uint8_t>((rgb_u32 >> 8) & 0xFF);
      cp.b = static_cast<uint8_t>(rgb_u32 & 0xFF);
    }
    out.push_back(cp);
  }
  return out;
}

}  // namespace mapping::ros2
