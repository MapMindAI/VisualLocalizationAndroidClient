#pragma once

#include "mapping/common/frame_protocol.h"

#include <opencv2/core/mat.hpp>

namespace vlprender {

void DrawOverlay(cv::Mat& image_bgr, const vlpstream::FramePacket& packet, double fps);

}  // namespace vlprender
