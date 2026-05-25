#include "mapping/common/data_session.h"

#include "mapping/common/utils.h"

#include <cstring>
#include <iostream>
#include <utility>

namespace mapping {
namespace {

constexpr char kFileMagic[] = "VLPREC1\n";
constexpr uint32_t kFileVersion = 1;
constexpr size_t kFrameHeaderSize = 64;
constexpr char kDepthTag[] = "DPT1";

}  // namespace

DataSessionReader::DataSessionReader(std::string path) : path_(std::move(path)) {}

bool DataSessionReader::Open() {
  ifs_.close();
  ifs_.clear();
  ifs_.open(path_, std::ios::binary);
  if (!ifs_.is_open()) {
    std::cerr << "Failed to open data session: " << path_ << std::endl;
    return false;
  }

  char magic[8] = {0};
  if (!ifs_.read(magic, sizeof(magic))) {
    std::cerr << "Failed to read file magic" << std::endl;
    return false;
  }
  uint32_t version = 0;
  if (!ifs_.read(reinterpret_cast<char*>(&version), sizeof(version))) {
    std::cerr << "Failed to read file version" << std::endl;
    return false;
  }
  if (std::memcmp(magic, kFileMagic, sizeof(magic)) != 0 || version != kFileVersion) {
    std::cerr << "Invalid session header: magic/version mismatch" << std::endl;
    return false;
  }
  return true;
}

bool DataSessionReader::Next(DataSessionFrame* frame) {
  if (frame == nullptr || !ifs_.is_open()) {
    return false;
  }

  for (;;) {
    uint64_t rel_ns_u64 = 0;
    uint32_t payload_size = 0;
    if (!ifs_.read(reinterpret_cast<char*>(&rel_ns_u64), sizeof(rel_ns_u64))) {
      return false;
    }
    if (!ifs_.read(reinterpret_cast<char*>(&payload_size), sizeof(payload_size))) {
      return false;
    }
    if (payload_size < kFrameHeaderSize) {
      std::cerr << "Skip invalid payload size=" << payload_size << std::endl;
      continue;
    }

    payload_buffer_.resize(payload_size);
    if (!ifs_.read(payload_buffer_.data(), static_cast<std::streamsize>(payload_size))) {
      return false;
    }

    auto packet_opt = vlpstream::ParseFramePayload(payload_buffer_);
    if (!packet_opt.has_value()) {
      continue;
    }

    frame->rel_ns = static_cast<int64_t>(rel_ns_u64);
    frame->packet = std::move(packet_opt.value());
    frame->depth_m.release();
    ExtractDepthFromPayload(payload_buffer_, &frame->depth_m);
    return true;
  }
}

bool DataSessionReader::ExtractDepthFromPayload(const std::string& payload, cv::Mat* depth_m) const {
  if (payload.size() < kFrameHeaderSize) {
    return false;
  }
  const uint8_t* data = reinterpret_cast<const uint8_t*>(payload.data());
  size_t jpg_end = std::string::npos;
  for (size_t i = kFrameHeaderSize + 1; i < payload.size(); ++i) {
    if (data[i - 1] == 0xFF && data[i] == 0xD9) {
      jpg_end = i + 1;
      break;
    }
  }
  if (jpg_end == std::string::npos || jpg_end <= kFrameHeaderSize) {
    return false;
  }

  if (depth_m == nullptr || jpg_end + 24 > payload.size()) {
    return true;
  }

  size_t cursor = jpg_end;
  while (cursor + 24 <= payload.size()) {
    if (std::memcmp(data + cursor, kDepthTag, 4) != 0) {
      break;
    }
    cursor += 4;
    cursor += 8;
    const uint32_t depth_w = vlputil::ReadU32LE(data + cursor);
    cursor += 4;
    const uint32_t depth_h = vlputil::ReadU32LE(data + cursor);
    cursor += 4;
    const uint32_t depth_size = vlputil::ReadU32LE(data + cursor);
    cursor += 4;
    if (cursor + depth_size > payload.size()) {
      break;
    }

    if (depth_w > 0 && depth_h > 0 && depth_size == depth_w * depth_h * 2u) {
      depth_m->create(static_cast<int>(depth_h), static_cast<int>(depth_w), CV_32F);
      const uint8_t* d = data + cursor;
      for (uint32_t y = 0; y < depth_h; ++y) {
        float* row = depth_m->ptr<float>(static_cast<int>(y));
        for (uint32_t x = 0; x < depth_w; ++x) {
          const size_t idx = static_cast<size_t>(y) * depth_w * 2u + static_cast<size_t>(x) * 2u;
          const uint16_t mm = static_cast<uint16_t>(d[idx]) |
                              (static_cast<uint16_t>(d[idx + 1]) << 8u);
          row[x] = mm > 0 ? static_cast<float>(mm) * 0.001f : 0.0f;
        }
      }
    }
    cursor += depth_size;
  }
  return true;
}

}  // namespace mapping
