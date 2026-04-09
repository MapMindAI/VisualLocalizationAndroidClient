/*
 * Copyright 2017 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hello_ar_application.h"

#include <android/asset_manager.h>

#include <array>

#include "arcore_c_api.h"
#include "plane_renderer.h"
#include "util.h"
#include "mobili_vlp_api.h"

namespace hello_ar {
namespace {
constexpr size_t kMaxNumberOfAndroidsToRender = 20;

const glm::vec3 kWhite = {255, 255, 255};

// Assumed distance from the device camera to the surface on which user will
// try to place objects. This value affects the apparent scale of objects
// while the tracking method of the Instant Placement point is
// SCREENSPACE_WITH_APPROXIMATE_DISTANCE. Values in the [0.2, 2.0] meter
// range are a good choice for most AR experiences. Use lower values for AR
// experiences where users are expected to place objects on surfaces close
// to the camera. Use larger values for experiences where the user will
// likely be standing and trying to place an object on the ground or floor
// in front of them.
constexpr float kApproximateDistanceMeters = 1.0f;

void SetColor(float r, float g, float b, float a, float* color4f) {
  color4f[0] = r;
  color4f[1] = g;
  color4f[2] = b;
  color4f[3] = a;
}

}  // namespace

HelloArApplication::HelloArApplication(AAssetManager* asset_manager)
    : asset_manager_(asset_manager) {

  LOGI("HelloArApplication start");
  mobili::vlp::VlpConfig config;
  config.vlp_address = "192.168.10.39:40010";
  config.update_with_covariance = true;
  config.buffer_max_spin = 10e9;
  config.vio_gravity_direction[0] = 0.0f;
  config.vio_gravity_direction[1] = 1.0f;
  config.vio_gravity_direction[2] = 0.0f;
  if (!mobili::vlp::StartVlpServer(config)) {
    LOGI("StartVlpServer failed!");
  }
}

HelloArApplication::~HelloArApplication() {
  if (ar_session_ != nullptr) {
    ArSession_destroy(ar_session_);
    ArFrame_destroy(ar_frame_);
  }
}

void HelloArApplication::OnPause() {
  LOGI("OnPause()");
  if (ar_session_ != nullptr) {
    ArSession_pause(ar_session_);
  }
}

void HelloArApplication::OnResume(JNIEnv* env, void* context, void* activity) {
  LOGI("OnResume()");

  if (ar_session_ == nullptr) {
    ArInstallStatus install_status;
    // If install was not yet requested, that means that we are resuming the
    // activity first time because of explicit user interaction (such as
    // launching the application)
    bool user_requested_install = !install_requested_;

    // === ATTENTION!  ATTENTION!  ATTENTION! ===
    // This method can and will fail in user-facing situations.  Your
    // application must handle these cases at least somewhat gracefully.  See
    // HelloAR Java sample code for reasonable behavior.
    CHECKANDTHROW(
        ArCoreApk_requestInstall(env, activity, user_requested_install,
                                 &install_status) == AR_SUCCESS,
        env, "Please install Google Play Services for AR (ARCore).");

    switch (install_status) {
      case AR_INSTALL_STATUS_INSTALLED:
        break;
      case AR_INSTALL_STATUS_INSTALL_REQUESTED:
        install_requested_ = true;
        return;
    }

    // === ATTENTION!  ATTENTION!  ATTENTION! ===
    // This method can and will fail in user-facing situations.  Your
    // application must handle these cases at least somewhat gracefully.  See
    // HelloAR Java sample code for reasonable behavior.
    CHECKANDTHROW(ArSession_create(env, context, &ar_session_) == AR_SUCCESS,
                  env, "Failed to create AR session.");

    ConfigureSession();
    ArFrame_create(ar_session_, &ar_frame_);

    ArSession_setDisplayGeometry(ar_session_, display_rotation_, width_,
                                 height_);
  }

  const ArStatus status = ArSession_resume(ar_session_);
  CHECKANDTHROW(status == AR_SUCCESS, env, "Failed to resume AR session.");
}

void HelloArApplication::OnSurfaceCreated() {
  LOGI("OnSurfaceCreated()");

  depth_texture_.CreateOnGlThread();
  background_renderer_.InitializeGlContent(asset_manager_,
                                           depth_texture_.GetTextureId());
  point_cloud_renderer_.InitializeGlContent(asset_manager_);
  andy_renderer_.InitializeGlContent(asset_manager_, "models/andy.obj",
                                     "models/andy.png");
  andy_renderer_.SetDepthTexture(depth_texture_.GetTextureId(),
                                 depth_texture_.GetWidth(),
                                 depth_texture_.GetHeight());

  // world_mesh_renderer_
  SetColor(73.0f / 255.0f, 73.0f / 255.0f, 130.0f / 255.0f, 100.0f / 255.0f, world_mesh_color_);
  world_mesh_renderer_.InitializeGlContent(asset_manager_, "/storage/emulated/0/Android/data/com.google.ar.core.examples.c.helloar/files/tsdf_mesh.obj");
  world_mesh_renderer_.SetDepthTexture(depth_texture_.GetTextureId(),
                                       depth_texture_.GetWidth(),
                                       depth_texture_.GetHeight());

  plane_renderer_.InitializeGlContent(asset_manager_);
}

void HelloArApplication::OnDisplayGeometryChanged(int display_rotation,
                                                  int width, int height) {
  LOGI("OnSurfaceChanged(%d, %d)", width, height);
  glViewport(0, 0, width, height);
  display_rotation_ = display_rotation;
  width_ = width;
  height_ = height;
  if (ar_session_ != nullptr) {
    ArSession_setDisplayGeometry(ar_session_, display_rotation, width, height);
  }

  ArCameraIntrinsics_create(ar_session_, &camera_intrinsics_);
}

int HelloArApplication::PublishImage() {
  if (publishing_) return -3;

  publishing_ = true;
  ArCamera* ar_camera;
  ArFrame_acquireCamera(ar_session_, ar_frame_, &ar_camera);
  ArCamera_getImageIntrinsics(ar_session_, ar_camera, camera_intrinsics_);

  float fx, fy;
  float cx, cy;
  int32_t width_intr, height_intr;
  ArCameraIntrinsics_getFocalLength(ar_session_, camera_intrinsics_, &fx, &fy);
  ArCameraIntrinsics_getPrincipalPoint(ar_session_, camera_intrinsics_, &cx, &cy);
  ArCameraIntrinsics_getImageDimensions(ar_session_, camera_intrinsics_, &width_intr, &height_intr);

  ArImage* image = nullptr;
  ArStatus status = ArFrame_acquireCameraImage(ar_session_, ar_frame_, &image);
  if (status != AR_SUCCESS) {
    // Image not available this frame.
    LOGI("Failed to acquire image.");
    publishing_ = false;
    return -4;
  }

  int width, height;
  int64_t image_ns;
  ArImage_getWidth(ar_session_, image, &width);
  ArImage_getHeight(ar_session_, image, &height);
  ArImage_getTimestamp(ar_session_, image, &image_ns);

  const uint8_t* y;
  const uint8_t* u;
  const uint8_t* v;
  int yLength, uLength, vLength;
  ArImage_getPlaneData(ar_session_, image, 0, &y, &yLength);
  ArImage_getPlaneData(ar_session_, image, 1, &u, &uLength);
  ArImage_getPlaneData(ar_session_, image, 2, &v, &vLength);

  // The image format is YUV_420_888, so you'll typically want to only save the Y (luminance) plane or convert to RGB for saving as PNG or JPG.
  // stride should be equals to width
  LOGI("camera intrinsics %ld (%f, %f, %f, %f, %d x %d)", image_ns, fx, fy, cx, cy, width_intr, height_intr);

  int ret;
  // std::string error_message;
  if (false) {
    uint8_t* yuv420 = new uint8_t[yLength + uLength + vLength];
    memcpy(yuv420, y, yLength);
    memcpy(yuv420 + yLength, u, uLength);
    memcpy(yuv420 + yLength + uLength, v, vLength);

    ret = mobili::vlp::PushImageYUV(image_ns, width, height, yuv420, yLength + uLength + vLength,
                                    fx, fy, cx, cy);
    delete[](yuv420);
  } else {
    ret = mobili::vlp::PushImageGray(image_ns, width, height, y, 0, fx, fy, cx, cy);
  }
  LOGI("vlp message %d", ret);

  ArImage_release(image);
  ArCamera_release(ar_camera);
  publishing_ = false;
  return ret;
}

bool HelloArApplication::popDebugMessage() { return mobili::vlp::PopLatestDebugMessage(nullptr); }

std::string HelloArApplication::getDebugMessage() { return mobili::vlp::GetLatestDebugMessage(); }

void HelloArApplication::OnDrawFrame(bool depthColorVisualizationEnabled,
                                     bool useDepthForOcclusion) {
  // Render the scene.
  glClearColor(0.9f, 0.9f, 0.9f, 1.0f);
  glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

  glEnable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);

  if (ar_session_ == nullptr) return;

  ArSession_setCameraTextureName(ar_session_,
                                 background_renderer_.GetTextureId());

  // Update session to get current frame and render camera background.
  if (ArSession_update(ar_session_, ar_frame_) != AR_SUCCESS) {
    LOGE("HelloArApplication::OnDrawFrame ArSession_update error");
  }

  andy_renderer_.SetDepthTexture(depth_texture_.GetTextureId(),
                                 depth_texture_.GetWidth(),
                                 depth_texture_.GetHeight());

  world_mesh_renderer_.SetDepthTexture(depth_texture_.GetTextureId(),
                                       depth_texture_.GetWidth(),
                                       depth_texture_.GetHeight());

  ArCamera* ar_camera;
  ArFrame_acquireCamera(ar_session_, ar_frame_, &ar_camera);

  int32_t geometry_changed = 0;
  ArFrame_getDisplayGeometryChanged(ar_session_, ar_frame_, &geometry_changed);
  if (geometry_changed != 0 || !calculate_uv_transform_) {
    // The UV Transform represents the transformation between screenspace in
    // normalized units and screenspace in units of pixels.  Having the size of
    // each pixel is necessary in the virtual object shader, to perform
    // kernel-based blur effects.
    calculate_uv_transform_ = false;
    glm::mat3 transform = GetTextureTransformMatrix(ar_session_, ar_frame_);
    andy_renderer_.SetUvTransformMatrix(transform);
    world_mesh_renderer_.SetUvTransformMatrix(transform);
  }

  glm::mat4 view_mat;
  glm::mat4 projection_mat;
  ArCamera_getViewMatrix(ar_session_, ar_camera, glm::value_ptr(view_mat));
  ArCamera_getProjectionMatrix(ar_session_, ar_camera,
                               /*near=*/0.01f, /*far=*/1000.f,
                               glm::value_ptr(projection_mat));

  if (mobili::vlp::Recording()) {
    ArCamera_getImageIntrinsics(ar_session_, ar_camera, camera_intrinsics_);
    float fx, fy;
    float cx, cy;
    ArCameraIntrinsics_getFocalLength(ar_session_, camera_intrinsics_, &fx, &fy);
    ArCameraIntrinsics_getPrincipalPoint(ar_session_, camera_intrinsics_, &cx, &cy);

    ArImage* image = nullptr;
    ArStatus status = ArFrame_acquireCameraImage(ar_session_, ar_frame_, &image);
    if (status != AR_SUCCESS) {  // Image not available this frame.
      LOGI("Failed to acquire image.");
    } else {
      int width, height;
      int64_t image_ns;
      ArImage_getTimestamp(ar_session_, image, &image_ns);
      ArImage_getWidth(ar_session_, image, &width);
      ArImage_getHeight(ar_session_, image, &height);

      const uint8_t* y;
      int yLength;
      ArImage_getPlaneData(ar_session_, image, 0, &y, &yLength);
      mobili::vlp::RecordImage(image_ns, width, height, y, fx, fy, cx, cy);
    }
    ArImage_release(image);
  }
  // push the pose to result
  {
    int64_t timestamp;
    ArFrame_getTimestamp(ar_session_, ar_frame_, &timestamp);

    // https://github.com/deepmirrorinc/CoreMap/blob/cb9d3db94893794d581135ad8ed30215d9071b5b/map/common/coordinate_conversion.h#L140
    glm::mat4 cam_world = glm::inverse(view_mat);
    glm::vec3 trans = glm::vec3(cam_world[3]);
    glm::quat quad_tmp = glm::quat_cast(cam_world);

    // the camera has a 90 deg additional rotation
    glm::quat offset_quad = glm::quat(glm::vec3(0.0f, 0.0f, glm::radians(-90.0f))); // Rotate 90° around Z-axis
    glm::quat quad = quad_tmp * offset_quad;

    mobili::vlp::PushVioCameraPose(timestamp, quad.x, quad.y, quad.z, quad.w, trans[0], trans[1], trans[2], mobili::vlp::OPENGL);
  }

  background_renderer_.Draw(ar_session_, ar_frame_,
                            depthColorVisualizationEnabled);

  ArTrackingState camera_tracking_state;
  ArCamera_getTrackingState(ar_session_, ar_camera, &camera_tracking_state);
  ArCamera_release(ar_camera);

  // If the camera isn't tracking don't bother rendering other objects.
  if (camera_tracking_state != AR_TRACKING_STATE_TRACKING) {
    return;
  }

  int32_t is_depth_supported = 0;
  ArSession_isDepthModeSupported(ar_session_, AR_DEPTH_MODE_AUTOMATIC,
                                 &is_depth_supported);
  if (is_depth_supported) {
    depth_texture_.UpdateWithDepthImageOnGlThread(*ar_session_, *ar_frame_);
  }

  // Get light estimation value.
  ArLightEstimate* ar_light_estimate;
  ArLightEstimateState ar_light_estimate_state;
  ArLightEstimate_create(ar_session_, &ar_light_estimate);

  ArFrame_getLightEstimate(ar_session_, ar_frame_, ar_light_estimate);
  ArLightEstimate_getState(ar_session_, ar_light_estimate,
                           &ar_light_estimate_state);

  // Set light intensity to default. Intensity value ranges from 0.0f to 1.0f.
  // The first three components are color scaling factors.
  // The last one is the average pixel intensity in gamma space.
  float color_correction[4] = {1.f, 1.f, 1.f, 1.f};
  if (ar_light_estimate_state == AR_LIGHT_ESTIMATE_STATE_VALID) {
    ArLightEstimate_getColorCorrection(ar_session_, ar_light_estimate,
                                       color_correction);
  }

  ArLightEstimate_destroy(ar_light_estimate);
  ar_light_estimate = nullptr;

  // Update and render planes.
  ArTrackableList* plane_list = nullptr;
  ArTrackableList_create(ar_session_, &plane_list);
  CHECK(plane_list != nullptr);

  ArTrackableType plane_tracked_type = AR_TRACKABLE_PLANE;
  ArSession_getAllTrackables(ar_session_, plane_tracked_type, plane_list);

  int32_t plane_list_size = 0;
  ArTrackableList_getSize(ar_session_, plane_list, &plane_list_size);
  plane_count_ = plane_list_size;

  for (int i = 0; i < plane_list_size; ++i) {
    ArTrackable* ar_trackable = nullptr;
    ArTrackableList_acquireItem(ar_session_, plane_list, i, &ar_trackable);
    ArPlane* ar_plane = ArAsPlane(ar_trackable);
    ArTrackingState out_tracking_state;
    ArTrackable_getTrackingState(ar_session_, ar_trackable,
                                 &out_tracking_state);

    ArPlane* subsume_plane;
    ArPlane_acquireSubsumedBy(ar_session_, ar_plane, &subsume_plane);
    if (subsume_plane != nullptr) {
      ArTrackable_release(ArAsTrackable(subsume_plane));
      ArTrackable_release(ar_trackable);
      continue;
    }

    if (ArTrackingState::AR_TRACKING_STATE_TRACKING != out_tracking_state) {
      ArTrackable_release(ar_trackable);
      continue;
    }

    plane_renderer_.Draw(projection_mat, view_mat, *ar_session_, *ar_plane);
    ArTrackable_release(ar_trackable);
  }

  ArTrackableList_destroy(plane_list);
  plane_list = nullptr;

  andy_renderer_.setUseDepthForOcclusion(asset_manager_, useDepthForOcclusion);
  world_mesh_renderer_.setUseDepthForOcclusion(asset_manager_, useDepthForOcclusion);

  // Render Andy objects.
  glm::mat4 model_mat(1.0f);
  for (auto& colored_anchor : anchors_) {
    ArTrackingState tracking_state = AR_TRACKING_STATE_STOPPED;
    ArAnchor_getTrackingState(ar_session_, colored_anchor.anchor,
                              &tracking_state);
    if (tracking_state == AR_TRACKING_STATE_TRACKING) {
      UpdateAnchorColor(&colored_anchor);
      // Render object only if the tracking state is AR_TRACKING_STATE_TRACKING.
      util::GetTransformMatrixFromAnchor(*colored_anchor.anchor, ar_session_,
                                         &model_mat);
      andy_renderer_.Draw(projection_mat, view_mat, model_mat, color_correction,
                          colored_anchor.color);
    }
  }
  {
    // set pose using vlp result
    int64_t timestamp;
    float world_to_local[7];
    if (GetLatestWorldToLocal(mobili::vlp::DEFAULT, &timestamp, world_to_local)) {
      // world_to_local : qx qy qz qw tx ty tz
      glm::quat quad(world_to_local[3], world_to_local[0], world_to_local[1], world_to_local[2]);
      glm::vec3 trans(world_to_local[4], world_to_local[5], world_to_local[6]);

      glm::mat4 model_mat(1.0f);
      glm::mat4 rot_mat = glm::toMat4(quad);          // Convert quaternion to rotation matrix
      model_mat = glm::translate(model_mat, trans); // Apply translation
      model_mat *= rot_mat;                               // Then apply rotation

      world_mesh_renderer_.Draw(projection_mat, view_mat, model_mat, color_correction,
                                world_mesh_color_);
    }
  }

  // Update and render point cloud.
  ArPointCloud* ar_point_cloud = nullptr;
  ArStatus point_cloud_status =
      ArFrame_acquirePointCloud(ar_session_, ar_frame_, &ar_point_cloud);
  if (point_cloud_status == AR_SUCCESS) {
    point_cloud_renderer_.Draw(projection_mat * view_mat, ar_session_,
                               ar_point_cloud);
    ArPointCloud_release(ar_point_cloud);
  }
}

bool HelloArApplication::IsDepthSupported() {
  int32_t is_supported = 0;
  ArSession_isDepthModeSupported(ar_session_, AR_DEPTH_MODE_AUTOMATIC,
                                 &is_supported);
  return is_supported;
}

void HelloArApplication::ConfigureSession() {
  const bool is_depth_supported = IsDepthSupported();

  ArConfig* ar_config = nullptr;
  ArConfig_create(ar_session_, &ar_config);
  if (is_depth_supported) {
    ArConfig_setDepthMode(ar_session_, ar_config, AR_DEPTH_MODE_AUTOMATIC);
  } else {
    ArConfig_setDepthMode(ar_session_, ar_config, AR_DEPTH_MODE_DISABLED);
  }

  if (is_instant_placement_enabled_) {
    ArConfig_setInstantPlacementMode(ar_session_, ar_config,
                                     AR_INSTANT_PLACEMENT_MODE_LOCAL_Y_UP);
  } else {
    ArConfig_setInstantPlacementMode(ar_session_, ar_config,
                                     AR_INSTANT_PLACEMENT_MODE_DISABLED);
  }
  CHECK(ar_config);
  CHECK(ArSession_configure(ar_session_, ar_config) == AR_SUCCESS);
  ArConfig_destroy(ar_config);
}

void HelloArApplication::OnSettingsChange(bool is_instant_placement_enabled) {
  is_instant_placement_enabled_ = is_instant_placement_enabled;

  if (ar_session_ != nullptr) {
    ConfigureSession();
  }
}

void HelloArApplication::OnTouched(float x, float y) {
  if (ar_frame_ != nullptr && ar_session_ != nullptr) {
    ArHitResultList* hit_result_list = nullptr;
    ArHitResultList_create(ar_session_, &hit_result_list);
    CHECK(hit_result_list);
    if (is_instant_placement_enabled_) {
      ArFrame_hitTestInstantPlacement(ar_session_, ar_frame_, x, y,
                                      kApproximateDistanceMeters,
                                      hit_result_list);
    } else {
      ArFrame_hitTest(ar_session_, ar_frame_, x, y, hit_result_list);
    }

    int32_t hit_result_list_size = 0;
    ArHitResultList_getSize(ar_session_, hit_result_list,
                            &hit_result_list_size);

    // The hitTest method sorts the resulting list by distance from the camera,
    // increasing.  The first hit result will usually be the most relevant when
    // responding to user input.

    ArHitResult* ar_hit_result = nullptr;
    for (int32_t i = 0; i < hit_result_list_size; ++i) {
      ArHitResult* ar_hit = nullptr;
      ArHitResult_create(ar_session_, &ar_hit);
      ArHitResultList_getItem(ar_session_, hit_result_list, i, ar_hit);

      if (ar_hit == nullptr) {
        LOGE("HelloArApplication::OnTouched ArHitResultList_getItem error");
        return;
      }

      ArTrackable* ar_trackable = nullptr;
      ArHitResult_acquireTrackable(ar_session_, ar_hit, &ar_trackable);
      ArTrackableType ar_trackable_type = AR_TRACKABLE_NOT_VALID;
      ArTrackable_getType(ar_session_, ar_trackable, &ar_trackable_type);
      // Creates an anchor if a plane or an oriented point was hit.
      if (AR_TRACKABLE_PLANE == ar_trackable_type) {
        ArPose* hit_pose = nullptr;
        ArPose_create(ar_session_, nullptr, &hit_pose);
        ArHitResult_getHitPose(ar_session_, ar_hit, hit_pose);
        int32_t in_polygon = 0;
        ArPlane* ar_plane = ArAsPlane(ar_trackable);
        ArPlane_isPoseInPolygon(ar_session_, ar_plane, hit_pose, &in_polygon);

        // Use hit pose and camera pose to check if hittest is from the
        // back of the plane, if it is, no need to create the anchor.
        ArPose* camera_pose = nullptr;
        ArPose_create(ar_session_, nullptr, &camera_pose);
        ArCamera* ar_camera;
        ArFrame_acquireCamera(ar_session_, ar_frame_, &ar_camera);
        ArCamera_getPose(ar_session_, ar_camera, camera_pose);
        ArCamera_release(ar_camera);
        float normal_distance_to_plane = util::CalculateDistanceToPlane(
            *ar_session_, *hit_pose, *camera_pose);

        ArPose_destroy(hit_pose);
        ArPose_destroy(camera_pose);

        if (!in_polygon || normal_distance_to_plane < 0) {
          continue;
        }

        ar_hit_result = ar_hit;
        break;
      } else if (AR_TRACKABLE_POINT == ar_trackable_type) {
        ArPoint* ar_point = ArAsPoint(ar_trackable);
        ArPointOrientationMode mode;
        ArPoint_getOrientationMode(ar_session_, ar_point, &mode);
        if (AR_POINT_ORIENTATION_ESTIMATED_SURFACE_NORMAL == mode) {
          ar_hit_result = ar_hit;
          break;
        }
      } else if (AR_TRACKABLE_INSTANT_PLACEMENT_POINT == ar_trackable_type) {
        ar_hit_result = ar_hit;
      } else if (AR_TRACKABLE_DEPTH_POINT == ar_trackable_type) {
        // ArDepthPoints are only returned if ArConfig_setDepthMode() is called
        // with AR_DEPTH_MODE_AUTOMATIC.
        ar_hit_result = ar_hit;
      }
    }

    if (ar_hit_result) {
      // Note that the application is responsible for releasing the anchor
      // pointer after using it. Call ArAnchor_release(anchor) to release.
      ArAnchor* anchor = nullptr;
      if (ArHitResult_acquireNewAnchor(ar_session_, ar_hit_result, &anchor) !=
          AR_SUCCESS) {
        LOGE(
            "HelloArApplication::OnTouched ArHitResult_acquireNewAnchor error");
        return;
      }

      ArTrackingState tracking_state = AR_TRACKING_STATE_STOPPED;
      ArAnchor_getTrackingState(ar_session_, anchor, &tracking_state);
      if (tracking_state != AR_TRACKING_STATE_TRACKING) {
        ArAnchor_release(anchor);
        return;
      }

      if (anchors_.size() >= kMaxNumberOfAndroidsToRender) {
        ArAnchor_release(anchors_[0].anchor);
        ArTrackable_release(anchors_[0].trackable);
        anchors_.erase(anchors_.begin());
      }

      ArTrackable* ar_trackable = nullptr;
      ArHitResult_acquireTrackable(ar_session_, ar_hit_result, &ar_trackable);
      // Assign a color to the object for rendering based on the trackable type
      // this anchor attached to. For AR_TRACKABLE_POINT, it's blue color, and
      // for AR_TRACKABLE_PLANE, it's green color.
      ColoredAnchor colored_anchor;
      colored_anchor.anchor = anchor;
      colored_anchor.trackable = ar_trackable;

      UpdateAnchorColor(&colored_anchor);
      anchors_.push_back(colored_anchor);

      ArHitResult_destroy(ar_hit_result);
      ar_hit_result = nullptr;

      ArHitResultList_destroy(hit_result_list);
      hit_result_list = nullptr;
    }
  }
}

void HelloArApplication::UpdateAnchorColor(ColoredAnchor* colored_anchor) {
  ArTrackable* ar_trackable = colored_anchor->trackable;
  float* color = colored_anchor->color;

  ArTrackableType ar_trackable_type;
  ArTrackable_getType(ar_session_, ar_trackable, &ar_trackable_type);

  if (ar_trackable_type == AR_TRACKABLE_POINT) {
    SetColor(66.0f, 133.0f, 244.0f, 255.0f, color);
    return;
  }

  if (ar_trackable_type == AR_TRACKABLE_PLANE) {
    SetColor(139.0f, 195.0f, 74.0f, 255.0f, color);
    return;
  }

  if (ar_trackable_type == AR_TRACKABLE_DEPTH_POINT) {
    SetColor(199.0f, 8.0f, 65.0f, 255.0f, color);
    return;
  }

  if (ar_trackable_type == AR_TRACKABLE_INSTANT_PLACEMENT_POINT) {
    ArInstantPlacementPoint* ar_instant_placement_point =
        ArAsInstantPlacementPoint(ar_trackable);
    ArInstantPlacementPointTrackingMethod tracking_method;
    ArInstantPlacementPoint_getTrackingMethod(
        ar_session_, ar_instant_placement_point, &tracking_method);
    if (tracking_method ==
        AR_INSTANT_PLACEMENT_POINT_TRACKING_METHOD_FULL_TRACKING) {
      SetColor(255.0f, 255.0f, 137.0f, 255.0f, color);
      return;
    } else if (
        tracking_method ==
        AR_INSTANT_PLACEMENT_POINT_TRACKING_METHOD_SCREENSPACE_WITH_APPROXIMATE_DISTANCE) {  // NOLINT
      SetColor(255.0f, 255.0f, 255.0f, 255.0f, color);
      return;
    }
  }

  // Fallback color
  SetColor(0.0f, 0.0f, 0.0f, 0.0f, color);
}

// This method returns a transformation matrix that when applied to screen space
// uvs makes them match correctly with the quad texture coords used to render
// the camera feed. It takes into account device orientation.
glm::mat3 HelloArApplication::GetTextureTransformMatrix(
    const ArSession* session, const ArFrame* frame) {
  float frameTransform[6];
  float uvTransform[9];
  // XY pairs of coordinates in NDC space that constitute the origin and points
  // along the two principal axes.
  const float ndcBasis[6] = {0, 0, 1, 0, 0, 1};
  ArFrame_transformCoordinates2d(
      session, frame, AR_COORDINATES_2D_OPENGL_NORMALIZED_DEVICE_COORDINATES, 3,
      ndcBasis, AR_COORDINATES_2D_TEXTURE_NORMALIZED, frameTransform);

  // Convert the transformed points into an affine transform and transpose it.
  float ndcOriginX = frameTransform[0];
  float ndcOriginY = frameTransform[1];
  uvTransform[0] = frameTransform[2] - ndcOriginX;
  uvTransform[1] = frameTransform[3] - ndcOriginY;
  uvTransform[2] = 0;
  uvTransform[3] = frameTransform[4] - ndcOriginX;
  uvTransform[4] = frameTransform[5] - ndcOriginY;
  uvTransform[5] = 0;
  uvTransform[6] = ndcOriginX;
  uvTransform[7] = ndcOriginY;
  uvTransform[8] = 1;

  return glm::make_mat3(uvTransform);
}
}  // namespace hello_ar
