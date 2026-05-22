// Copyright 2025 DeepMirror Inc. All rights reserved.

#ifndef EXPORT_MAPMIND_VLP_API_H_
#define EXPORT_MAPMIND_VLP_API_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace mapmind {
namespace vlp {

// Start/stop a gRPC server exposing:
//   /vlp.FrameStreamService/StreamFrames (unary-stream, raw bytes)
bool StartGrpcServer(int port);
void StopGrpcServer();
bool HasGrpcServer();

// Publish one full VLP2 payload to current subscribers.
// payload format should match app stream format:
// [64-byte VLP2 header][jpeg bytes]
void PushFramePayload(int64_t frame_timestamp_ns, const std::string& payload);

// Build and publish one VLP2 payload using NV21 image bytes.
// Body format: raw NV21 bytes.
void PushFrameYuvNv21(int64_t frame_timestamp_ns, int width, int height,
                      const uint8_t* yuv_nv21, size_t yuv_nv21_size, float fx, float fy,
                      float cx, float cy, float qx, float qy, float qz, float qw,
                      float tx, float ty, float tz);

// Publish latest computed depth (e.g. depth_rg, 16-bit mm packed as 2x8-bit RG)
// associated with RGB timestamp. Depth will be attached once to a subsequent
// frame payload when available.
void PushDepthUpdate(int64_t rgb_timestamp_ns, int width, int height,
                     const uint8_t* depth_bytes, size_t depth_size);

// Record raw payload stream in VLPREC1 format.
bool StartRecording(const std::string& output_file_path);
bool Recording();
void StopRecording();

// Enable/disable internal logs (enabled by default).
void SetLogsEnabled(bool enabled);

}  // namespace vlp
}  // namespace mapmind

#endif  // EXPORT_MAPMIND_VLP_API_H_
