// Copyright 2025 DeepMirror Inc. All rights reserved.

#ifndef EXPORT_MOBILI_VLP_API_H_
#define EXPORT_MOBILI_VLP_API_H_

#include <string>
#include <vector>

namespace mobili::vlp {

struct VlpConfig {
  std::string vlp_address;
  bool update_with_covariance = true;
  int64_t buffer_max_spin = 5e9;
  // i.e. the upper direction in earth, PLEASE CHECK YOUR COORDINATE SYSTEM
  float vio_gravity_direction[3] = {0.0f, 1.0f, 0.0f};
};

bool StartVlpServer(const VlpConfig& config);
void StopVlpServer();

bool StartRecording(const std::string& record_folder);
bool Recording();
void StopRecording();
void RecordImage(int64_t timestamp, int width, int height, const uint8_t* data, float fx, float fy,
                 float cx, float cy);

// return values :
//  * -2 : not init
//  * -1 : vlp already processing
//  * 1 : success
int PushImageJPG(int64_t timestamp, int width, int height, uint8_t* data, size_t data_length,
                 float fx, float fy, float cx, float cy);
int PushImageYUV(int64_t timestamp, int width, int height, uint8_t* data, size_t data_length,
                 float fx, float fy, float cx, float cy);
int PushImageGray(int64_t timestamp, int width, int height, const uint8_t* data, size_t data_length,
                  float fx, float fy, float cx, float cy);
int PushImageR8G8B8A8(int64_t timestamp, int width, int height, const uint8_t* data, int stride,
                      float resize_ratio, float fx, float fy, float cx, float cy);
int PushImageProto(void* proto_void);
void GetLastCameraToLocal(void* pose_se3f);

// https://github.com/deepmirrorinc/CoreMap/blob/cb9d3db94893794d581135ad8ed30215d9071b5b/map/common/coordinate_conversion.h#L140
enum PoseType {
  DEFAULT = 0,
  // Camera Coordinate System.
  //              /| z (forward)
  //             /
  //            /
  //           -----------> x(right)
  //           |
  //           |
  //           |
  //           |/ y (down)

  OPENGL = 1,
  // OpenGL Device Coordinate System. This is used by native Android applications.
  //              |\ y (up)
  //              |
  //              |
  //              |
  //              -----------> x(right)
  //             /
  //            /
  //           /
  //         |/ z (backward)

  UNITY = 2,
  // Unity Device Coordinate System. This is used by Unity applications. The difference between
  // UNITY coordinate system and OPEN_GL coordinate system is that Z axis is flipped.
  //           y (up) |\     z (forward)
  //                  |  /|
  //                  | /
  //                  |/
  //                  ---------> x (right)

  UNREAL = 3
};
void PushVioCameraPose(int64_t timestamp, float qx, float qy, float qz, float qw, float tx,
                       float ty, float tz, PoseType pose_type);

// world_to_local : qx qy qz qw tx ty tz
bool GetLatestWorldToLocal(PoseType pose_type, int64_t* timestamp, float* world_to_local);

std::string GetLatestDebugMessage();
bool PopLatestDebugMessage(std::string* message);

}  // namespace mobili::vlp

#endif  // EXPORT_MOBILI_VLP_API_H_
