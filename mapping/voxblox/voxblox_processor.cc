#include "mapping/voxblox/voxblox_processor.h"

#include <voxblox/core/common.h>
#include <voxblox/core/esdf_map.h>
#include <voxblox/core/tsdf_map.h>
#include <voxblox/integrator/esdf_integrator.h>
#include <voxblox/integrator/tsdf_integrator.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace mapping {
namespace {

inline bool InSlice(const voxblox::Point& coord, unsigned int free_plane_index,
                    float free_plane_val, float voxel_size) {
  return std::abs(coord(free_plane_index) - free_plane_val) <=
         (voxel_size / 2.0f + 1e-6f);
}

inline voxblox::Color EsdfDistanceColor(float distance_m) {
  // Same style as reference: color from signed distance.
  return voxblox::rainbowColorMap(static_cast<double>(distance_m) * 0.5);
}

}  // namespace

struct VoxbloxProcessor::Impl {
  explicit Impl(const Config& cfg) : config(cfg) {
    voxblox::TsdfMap::Config tsdf_cfg;
    tsdf_cfg.tsdf_voxel_size = config.voxel_size_m;
    tsdf_cfg.tsdf_voxels_per_side = static_cast<size_t>(std::max(1, config.voxels_per_side));
    tsdf_map = std::make_unique<voxblox::TsdfMap>(tsdf_cfg);

    voxblox::TsdfIntegratorBase::Config tsdf_int_cfg;
    tsdf_int_cfg.default_truncation_distance = config.truncation_distance_m;
    tsdf_int_cfg.min_ray_length_m = config.min_ray_length_m;
    tsdf_int_cfg.max_ray_length_m = config.max_ray_length_m;
    tsdf_int_cfg.voxel_carving_enabled = true;
    tsdf_integrator = std::make_unique<voxblox::FastTsdfIntegrator>(
        tsdf_int_cfg, tsdf_map->getTsdfLayerPtr());

    voxblox::EsdfMap::Config esdf_cfg;
    esdf_cfg.esdf_voxel_size = config.voxel_size_m;
    esdf_cfg.esdf_voxels_per_side = static_cast<size_t>(std::max(1, config.voxels_per_side));
    esdf_map = std::make_unique<voxblox::EsdfMap>(esdf_cfg);

    voxblox::EsdfIntegrator::Config esdf_int_cfg;
    esdf_int_cfg.max_distance_m = config.esdf_max_distance_m;
    esdf_int_cfg.default_distance_m = config.esdf_max_distance_m;
    esdf_integrator = std::make_unique<voxblox::EsdfIntegrator>(
        esdf_int_cfg, tsdf_map->getTsdfLayerPtr(), esdf_map->getEsdfLayerPtr());
  }

  void UpdateEsdfMap(bool full_update) {
    if (!tsdf_map || !esdf_integrator) {
      return;
    }
    if (tsdf_map->getTsdfLayer().getNumberOfAllocatedBlocks() == 0) {
      return;
    }
    // clear_updated_flag_esdf
    esdf_integrator->updateFromTsdfLayer(true);
    if (full_update) {
      esdf_integrator->updateFromTsdfLayerBatch();
    }
  }

  bool Integrate(const cv::Mat& depth_m, const Pose& T_w_c, float fx, float fy, float cx,
                 float cy) {
    if (depth_m.empty() || depth_m.type() != CV_32F || fx <= 0.0f || fy <= 0.0f) {
      return false;
    }

    const int step = std::max(1, config.pixel_step);
    voxblox::Pointcloud points_C;
    voxblox::Colors colors;
    points_C.reserve(static_cast<size_t>(depth_m.rows / step) *
                     static_cast<size_t>(depth_m.cols / step));
    colors.reserve(points_C.capacity());

    for (int v = 0; v < depth_m.rows; v += step) {
      const float* depth_row = depth_m.ptr<float>(v);
      for (int u = 0; u < depth_m.cols; u += step) {
        const float z = depth_row[u];
        if (!std::isfinite(z) || z <= 0.05f || z > config.max_depth_m) {
          continue;
        }
        const float x = (static_cast<float>(u) - cx) * z / fx;
        const float y = (static_cast<float>(v) - cy) * z / fy;
        points_C.emplace_back(x, y, z);
        colors.emplace_back(200, 200, 200);
      }
    }

    if (points_C.empty()) {
      return false;
    }

    const Eigen::Quaternionf q_w_c = T_w_c.unit_quaternion();
    const voxblox::Point t_w_c = T_w_c.translation();
    const voxblox::Transformation T_G_C(q_w_c, t_w_c);

    tsdf_integrator->integratePointCloud(T_G_C, points_C, colors, false);
    ++integrated_frames;

    bool full_update = false;
    if (config.esdf_full_update_every_n > 0) {
      full_update = (integrated_frames % config.esdf_full_update_every_n) == 0;
    }
    UpdateEsdfMap(full_update);
    return true;
  }

  void GetTsdfVisualization(std::vector<VizPoint>* points) const {
    if (points == nullptr) {
      return;
    }
    points->clear();
    if (!tsdf_map) {
      return;
    }
    const auto* layer = tsdf_map->getTsdfLayerConstPtr();
    voxblox::BlockIndexList blocks;
    layer->getAllAllocatedBlocks(&blocks);
    const int voxel_step = std::max(1, config.viz_voxel_step);
    points->reserve(static_cast<size_t>(config.max_tsdf_viz_points));

    for (const voxblox::BlockIndex& block_idx : blocks) {
      auto block_ptr = layer->getBlockPtrByIndex(block_idx);
      if (!block_ptr) {
        continue;
      }
      const size_t n = block_ptr->num_voxels();
      for (size_t i = 0; i < n; i += static_cast<size_t>(voxel_step)) {
        const voxblox::TsdfVoxel& voxel = block_ptr->getVoxelByLinearIndex(i);
        if (voxel.weight < config.tsdf_min_weight ||
            std::abs(voxel.distance) > config.tsdf_surface_band_m) {
          continue;
        }
        const voxblox::Point p = block_ptr->computeCoordinatesFromLinearIndex(i);
        VizPoint vp;
        vp.x = p.x();
        vp.y = p.y();
        vp.z = p.z();
        vp.r = static_cast<float>(voxel.color.r);
        vp.g = static_cast<float>(voxel.color.g);
        vp.b = static_cast<float>(voxel.color.b);
        points->push_back(vp);
        if (static_cast<int>(points->size()) >= config.max_tsdf_viz_points) {
          return;
        }
      }
    }
  }

  void GetEsdfVisualization(std::vector<VizPoint>* points) const {
    if (points == nullptr) {
      return;
    }
    points->clear();
    if (!esdf_map) {
      return;
    }

    const auto& layer = esdf_map->getEsdfLayer();
    voxblox::BlockIndexList blocks;
    layer.getAllAllocatedBlocks(&blocks);
    const int voxel_step = std::max(1, config.viz_voxel_step);
    points->reserve(static_cast<size_t>(config.max_esdf_viz_points));

    float free_plane_val = config.esdf_slice_level_m;
    if (std::remainder(free_plane_val, layer.voxel_size()) < 1e-6f) {
      free_plane_val += layer.voxel_size() / 2.0f;
    }
    const unsigned int slice_axis =
        static_cast<unsigned int>(std::max(0, std::min(2, config.esdf_slice_axis)));

    for (const voxblox::BlockIndex& block_idx : blocks) {
      auto block_ptr = layer.getBlockPtrByIndex(block_idx);
      if (!block_ptr) {
        continue;
      }
      const size_t n = block_ptr->num_voxels();
      for (size_t i = 0; i < n; i += static_cast<size_t>(voxel_step)) {
        const voxblox::EsdfVoxel& voxel = block_ptr->getVoxelByLinearIndex(i);
        if (!std::isfinite(voxel.distance) ||
            std::abs(voxel.distance) > config.esdf_vis_distance_m) {
          continue;
        }
        if (!config.esdf_show_free && !voxel.observed) {
          continue;
        }
        if (config.esdf_only_occupied && voxel.distance > 0.0f) {
          continue;
        }

        const voxblox::Point p = block_ptr->computeCoordinatesFromLinearIndex(i);
        if (config.esdf_use_slice &&
            !InSlice(p, slice_axis, free_plane_val, layer.voxel_size())) {
          continue;
        }

        const voxblox::Color c = EsdfDistanceColor(voxel.distance);
        VizPoint vp;
        vp.x = p.x();
        vp.y = p.y();
        vp.z = p.z();
        vp.r = static_cast<float>(c.b);
        vp.g = static_cast<float>(c.g);
        vp.b = static_cast<float>(c.r);
        points->push_back(vp);
        if (static_cast<int>(points->size()) >= config.max_esdf_viz_points) {
          return;
        }
      }
    }
  }

  Config config;
  int integrated_frames = 0;
  std::unique_ptr<voxblox::TsdfMap> tsdf_map;
  std::unique_ptr<voxblox::EsdfMap> esdf_map;
  std::unique_ptr<voxblox::TsdfIntegratorBase> tsdf_integrator;
  std::unique_ptr<voxblox::EsdfIntegrator> esdf_integrator;
};

VoxbloxProcessor::VoxbloxProcessor(const Config& config)
    : impl_(std::make_unique<Impl>(config)) {}

VoxbloxProcessor::~VoxbloxProcessor() = default;

bool VoxbloxProcessor::Integrate(const cv::Mat& depth_m, const Pose& T_w_c, float fx,
                                 float fy, float cx, float cy) {
  return impl_ && impl_->Integrate(depth_m, T_w_c, fx, fy, cx, cy);
}

void VoxbloxProcessor::GetTsdfVisualization(std::vector<VizPoint>* points) const {
  if (impl_) {
    impl_->GetTsdfVisualization(points);
  }
}

void VoxbloxProcessor::GetEsdfVisualization(std::vector<VizPoint>* points) const {
  if (impl_) {
    impl_->GetEsdfVisualization(points);
  }
}

int VoxbloxProcessor::IntegratedFrameCount() const {
  return impl_ ? impl_->integrated_frames : 0;
}

}  // namespace mapping
