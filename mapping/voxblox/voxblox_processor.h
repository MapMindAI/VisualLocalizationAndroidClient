#pragma once

#include "mapping/common/pose_types.h"

#include <opencv2/core/mat.hpp>

#include <Eigen/Core>
#include <memory>
#include <vector>

namespace mapping {

class VoxbloxProcessor {
 public:
  struct VizPoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float r = 255.0f;
    float g = 255.0f;
    float b = 255.0f;
    float v = 0.0f;  // Signed ESDF distance value for client-side color mapping.
  };

  struct EsdfPlane2D {
    int width = 0;
    int height = 0;
    float resolution_m = 0.0f;
    float origin_x_m = 0.0f;
    float origin_z_m = 0.0f;
    float plane_height_m = 0.0f;
    std::vector<float> distances;  // Row-major [row=z][col=x], meters.
  };

  struct Config {
    Config(float _voxel_size_m): voxel_size_m(_voxel_size_m) {
      truncation_distance_m = voxel_size_m * 2;
      esdf_vis_distance_m = voxel_size_m * 3;
    }
    float voxel_size_m = 0.1f;
    int voxels_per_side = 16;
    float truncation_distance_m = 0.2f;
    float min_ray_length_m = 0.1f;
    float max_ray_length_m = 5.0f;
    float max_depth_m = 2.0f;
    int pixel_step = 4;
    float esdf_max_distance_m = 2.0f;
    bool esdf_show_free = false;
    bool esdf_only_occupied = true;
    int esdf_slice_axis = 2;
    float esdf_slice_level_m = 0.0f;
    int esdf_full_update_every_n = 20;
    float tsdf_surface_band_m = 0.08f;
    float tsdf_min_weight = 1.0f;
    float esdf_vis_distance_m = 1.0f;
    int viz_voxel_step = 1;
    int max_tsdf_viz_points = -1;
    int max_esdf_viz_points = -1;
  };

  explicit VoxbloxProcessor(const Config& config);
  ~VoxbloxProcessor();

  VoxbloxProcessor(const VoxbloxProcessor&) = delete;
  VoxbloxProcessor& operator=(const VoxbloxProcessor&) = delete;

  bool Integrate(const cv::Mat& depth_m, const Pose& T_w_c, float fx, float fy, float cx,
                 float cy);
  bool IntegratePointCloud(const std::vector<Eigen::Vector3f>& points_c, const Pose& T_w_c);
  void GetTsdfVisualization(std::vector<VizPoint>* points) const;
  void GetEsdfVisualization(std::vector<VizPoint>* points) const;
  bool GetEsdfPlaneSlice2D(float plane_height_m, EsdfPlane2D* out, int max_cells) const;

  int IntegratedFrameCount() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mapping
