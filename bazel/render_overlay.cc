#include "render_overlay.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace vlprender {

void DrawOverlay(cv::Mat& image_bgr, const vlpstream::FramePacket& packet, double fps) {
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
      cv::format("pose q=(%.4f, %.4f, %.4f, %.4f)", packet.pose.qx, packet.pose.qy,
                 packet.pose.qz, packet.pose.qw),
      cv::format("pose t=(%.4f, %.4f, %.4f)", packet.pose.tx, packet.pose.ty, packet.pose.tz),
      "press q to quit",
  };

  int y = 28;
  for (const auto& line : lines) {
    cv::putText(image_bgr, line, cv::Point(10, y), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    y += 26;
  }
}

}  // namespace vlprender
