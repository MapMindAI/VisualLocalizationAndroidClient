// Copyright 2025 DeepMirror Inc. All rights reserved.

#ifndef EXPORT_MAPMIND_VLP_API_H_
#define EXPORT_MAPMIND_VLP_API_H_

#include <cstdint>
#include <string>

namespace mapmind::vlp {

// Start/stop a gRPC server exposing:
//   /vlp.FrameStreamService/StreamFrames (unary-stream, raw bytes)
bool StartGrpcServer(int port);
void StopGrpcServer();
bool HasGrpcServer();

// Publish one full VLP2 payload to current subscribers.
// payload format should match app stream format:
// [64-byte VLP2 header][jpeg bytes]
void PushFramePayload(int64_t frame_timestamp_ns, const std::string& payload);

// Record raw payload stream in VLPREC1 format.
bool StartRecording(const std::string& output_file_path);
bool Recording();
void StopRecording();

}  // namespace mapmind::vlp

#endif  // EXPORT_MAPMIND_VLP_API_H_

