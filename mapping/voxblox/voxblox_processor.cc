#include "mapping/voxblox/voxblox_processor.h"

#include <voxblox/core/common.h>
#include <voxblox/core/esdf_map.h>
#include <voxblox/core/tsdf_map.h>
#include <voxblox/integrator/esdf_integrator.h>
#include <voxblox/integrator/tsdf_integrator.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
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
  struct BlockKey {
    int x = 0;
    int y = 0;
    int z = 0;
    bool operator==(const BlockKey& o) const { return x == o.x && y == o.y && z == o.z; }
  };
  struct BlockKeyHash {
    size_t operator()(const BlockKey& k) const {
      size_t h = static_cast<size_t>(k.x) * 73856093u;
      h ^= static_cast<size_t>(k.y) * 19349663u;
      h ^= static_cast<size_t>(k.z) * 83492791u;
      return h;
    }
  };

  explicit Impl(const Config& cfg) : config(cfg) {
    voxblox::TsdfMap::Config tsdf_cfg;
    tsdf_cfg.tsdf_voxel_size = config.voxel_size_m;
    tsdf_map = std::make_unique<voxblox::TsdfMap>(tsdf_cfg);

    voxblox::TsdfIntegratorBase::Config tsdf_int_cfg;
    tsdf_int_cfg.default_truncation_distance = config.truncation_distance_m;
    tsdf_int_cfg.min_ray_length_m = config.min_ray_length_m;
    tsdf_int_cfg.max_ray_length_m = config.max_ray_length_m;
    tsdf_int_cfg.voxel_carving_enabled = true;
    tsdf_int_cfg.use_const_weight = false;

    // allow_clear lets points beyond max_ray_length_m clear up to that distance
    tsdf_int_cfg.allow_clear = false;
    tsdf_integrator = std::make_unique<voxblox::MergedTsdfIntegrator>(
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
    points_C.reserve(static_cast<size_t>(depth_m.rows / step) *
                     static_cast<size_t>(depth_m.cols / step));

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
      }
    }
    return IntegratePointCloud(points_C, T_w_c);
  }

  bool IntegratePointCloud(const voxblox::Pointcloud& points_C, const Pose& T_w_c) {
    if (points_C.empty()) {
      return false;
    }
    voxblox::Colors colors;
    colors.resize(points_C.size());
    std::fill(colors.begin(), colors.end(), voxblox::Color(200, 200, 200));
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
    latest_pose_t = T_w_c.translation();
    has_latest_pose = true;
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
    if (config.max_tsdf_viz_points > 0) {
      points->reserve(static_cast<size_t>(config.max_tsdf_viz_points));
    }

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
        if (config.max_tsdf_viz_points > 0 && static_cast<int>(points->size()) >= config.max_tsdf_viz_points) {
          return;
        }
      }
    }
  }

  void RefreshNearbyEsdfBlocks() const {
    if (!esdf_map || !has_latest_pose) return;
    const auto& layer = esdf_map->getEsdfLayer();
    voxblox::BlockIndexList blocks;
    layer.getAllAllocatedBlocks(&blocks);
    if (blocks.empty()) return;
    const float r2 = config.esdf_vis_update_radius_m * config.esdf_vis_update_radius_m;
    // always refresh nearby blocks only.
    for (const auto& idx : blocks) {
      const auto block_ptr = layer.getBlockPtrByIndex(idx);
      if (!block_ptr) continue;
      const voxblox::Point center =
          block_ptr->origin() + voxblox::Point::Constant(0.5f * layer.block_size());
      const float dx = center.x() - latest_pose_t.x();
      const float dy = center.y() - latest_pose_t.y();
      const float dz = center.z() - latest_pose_t.z();
      if (dx * dx + dy * dy + dz * dz > r2) continue;
      UpdateCachedBlock(idx, *block_ptr);
    }
  }

  static BlockKey ToBlockKey(const voxblox::BlockIndex& idx) {
    return BlockKey{idx.x(), idx.y(), idx.z()};
  }

  void UpdateCachedBlock(const voxblox::BlockIndex& idx,
                         const voxblox::Block<voxblox::EsdfVoxel>& block) const {
    std::vector<VizPoint> out;
    const size_t n = block.num_voxels();
    const int voxel_step = std::max(1, config.viz_voxel_step);
    for (size_t i = 0; i < n; i += static_cast<size_t>(voxel_step)) {
      const voxblox::EsdfVoxel& voxel = block.getVoxelByLinearIndex(i);
      if (!std::isfinite(voxel.distance) || std::abs(voxel.distance) > config.esdf_vis_distance_m) {
        continue;
      }
      if (!config.esdf_show_free && !voxel.observed) {
        continue;
      }
      if (config.esdf_only_occupied && voxel.distance > 0.0f) {
        continue;
      }
      const voxblox::Point p = block.computeCoordinatesFromLinearIndex(i);
      const voxblox::Color c = EsdfDistanceColor(voxel.distance);
      VizPoint vp;
      vp.x = p.x();
      vp.y = p.y();
      vp.z = p.z();
      vp.r = static_cast<float>(c.b);
      vp.g = static_cast<float>(c.g);
      vp.b = static_cast<float>(c.r);
      vp.v = voxel.distance;
      out.push_back(vp);
    }
    esdf_block_cache[ToBlockKey(idx)] = std::move(out);
  }

  void RefreshAllEsdfBlocks() const {
    if (!esdf_map) return;
    const auto& layer = esdf_map->getEsdfLayer();
    voxblox::BlockIndexList blocks;
    layer.getAllAllocatedBlocks(&blocks);
    for (const auto& idx : blocks) {
      const auto block_ptr = layer.getBlockPtrByIndex(idx);
      if (!block_ptr) continue;
      UpdateCachedBlock(idx, *block_ptr);
    }
  }

  void GetEsdfVisualization(std::vector<VizPoint>* points, bool get_full) const {
    if (points == nullptr) {
      return;
    }
    points->clear();
    if (!esdf_map) {
      return;
    }
    if (get_full) {
      RefreshAllEsdfBlocks();
    } else {
      RefreshNearbyEsdfBlocks();
    }
    size_t total = 0;
    for (const auto& kv : esdf_block_cache) {
      total += kv.second.size();
    }
    if (config.max_esdf_viz_points > 0) {
      total = std::min<size_t>(total, static_cast<size_t>(config.max_esdf_viz_points));
    }
    points->reserve(total);
    for (const auto& kv : esdf_block_cache) {
      for (const auto& p : kv.second) {
        points->push_back(p);
        if (config.max_esdf_viz_points > 0 &&
            static_cast<int>(points->size()) >= config.max_esdf_viz_points) {
          return;
        }
      }
    }
  }

  bool GetEsdfPlaneSlice2D(float plane_height_m, EsdfPlane2D* out, int max_cells) const {
    if (out == nullptr || !esdf_map) {
      return false;
    }
    out->width = 0;
    out->height = 0;
    out->distances.clear();
    out->resolution_m = config.voxel_size_m;
    out->origin_x_m = 0.0f;
    out->origin_z_m = 0.0f;
    out->plane_height_m = plane_height_m;

    const auto& layer = esdf_map->getEsdfLayer();
    voxblox::BlockIndexList blocks;
    layer.getAllAllocatedBlocks(&blocks);
    if (blocks.empty()) {
      return false;
    }

    const float half_thickness = config.voxel_size_m * 0.5f + 1e-6f;
    float min_x = std::numeric_limits<float>::infinity();
    float max_x = -std::numeric_limits<float>::infinity();
    float min_z = std::numeric_limits<float>::infinity();
    float max_z = -std::numeric_limits<float>::infinity();
    bool has_samples = false;

    for (const voxblox::BlockIndex& block_idx : blocks) {
      auto block_ptr = layer.getBlockPtrByIndex(block_idx);
      if (!block_ptr) {
        continue;
      }
      const size_t n = block_ptr->num_voxels();
      for (size_t i = 0; i < n; ++i) {
        const voxblox::EsdfVoxel& voxel = block_ptr->getVoxelByLinearIndex(i);
        if (!std::isfinite(voxel.distance) || !voxel.observed) {
          continue;
        }
        const voxblox::Point p = block_ptr->computeCoordinatesFromLinearIndex(i);
        if (std::abs(p.y() - plane_height_m) > half_thickness) {
          continue;
        }
        min_x = std::min(min_x, p.x());
        max_x = std::max(max_x, p.x());
        min_z = std::min(min_z, p.z());
        max_z = std::max(max_z, p.z());
        has_samples = true;
      }
    }

    if (!has_samples) {
      return false;
    }

    const float voxel_size = config.voxel_size_m;
    const float eps = voxel_size * 1e-4f;
    const int width = static_cast<int>(std::floor((max_x - min_x) / voxel_size + 1.0f + eps));
    const int height = static_cast<int>(std::floor((max_z - min_z) / voxel_size + 1.0f + eps));
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (max_cells > 0 && static_cast<int64_t>(width) * static_cast<int64_t>(height) > max_cells) {
      return false;
    }

    out->width = width;
    out->height = height;
    out->origin_x_m = min_x;
    out->origin_z_m = min_z;
    out->distances.assign(static_cast<size_t>(width) * static_cast<size_t>(height),
                          std::numeric_limits<float>::quiet_NaN());

    for (const voxblox::BlockIndex& block_idx : blocks) {
      auto block_ptr = layer.getBlockPtrByIndex(block_idx);
      if (!block_ptr) {
        continue;
      }
      const size_t n = block_ptr->num_voxels();
      for (size_t i = 0; i < n; ++i) {
        const voxblox::EsdfVoxel& voxel = block_ptr->getVoxelByLinearIndex(i);
        if (!std::isfinite(voxel.distance) || !voxel.observed) {
          continue;
        }
        const voxblox::Point p = block_ptr->computeCoordinatesFromLinearIndex(i);
        if (std::abs(p.y() - plane_height_m) > half_thickness) {
          continue;
        }
        const int x = static_cast<int>(std::floor((p.x() - min_x) / voxel_size + eps));
        const int z = static_cast<int>(std::floor((p.z() - min_z) / voxel_size + eps));
        if (x < 0 || x >= width || z < 0 || z >= height) {
          continue;
        }
        out->distances[static_cast<size_t>(z) * static_cast<size_t>(width) +
                       static_cast<size_t>(x)] = voxel.distance;
      }
    }

    return true;
  }

  Config config;
  int integrated_frames = 0;
  std::unique_ptr<voxblox::TsdfMap> tsdf_map;
  std::unique_ptr<voxblox::EsdfMap> esdf_map;
  std::unique_ptr<voxblox::TsdfIntegratorBase> tsdf_integrator;
  std::unique_ptr<voxblox::EsdfIntegrator> esdf_integrator;
  mutable std::unordered_map<BlockKey, std::vector<VizPoint>, BlockKeyHash> esdf_block_cache;
  mutable Eigen::Vector3f latest_pose_t = Eigen::Vector3f::Zero();
  mutable bool has_latest_pose = false;
};

VoxbloxProcessor::VoxbloxProcessor(const Config& config)
    : impl_(std::make_unique<Impl>(config)) {}

VoxbloxProcessor::~VoxbloxProcessor() = default;

bool VoxbloxProcessor::Integrate(const cv::Mat& depth_m, const Pose& T_w_c, float fx,
                                 float fy, float cx, float cy) {
  return impl_ && impl_->Integrate(depth_m, T_w_c, fx, fy, cx, cy);
}

bool VoxbloxProcessor::IntegratePointCloud(const std::vector<Eigen::Vector3f>& points_c,
                                           const Pose& T_w_c) {
  if (!impl_) {
    return false;
  }
  voxblox::Pointcloud points;
  points.resize(points_c.size());
  static_assert(sizeof(voxblox::Point) == sizeof(Eigen::Vector3f),
                "voxblox::Point and Eigen::Vector3f size mismatch");
  if (!points_c.empty()) {
    std::memcpy(points[0].data(), points_c.data(), points_c.size() * sizeof(Eigen::Vector3f));
  }
  return impl_->IntegratePointCloud(points, T_w_c);
}

void VoxbloxProcessor::GetTsdfVisualization(std::vector<VizPoint>* points) const {
  if (impl_) {
    impl_->GetTsdfVisualization(points);
  }
}

void VoxbloxProcessor::GetEsdfVisualization(std::vector<VizPoint>* points, bool get_full) const {
  if (impl_) {
    impl_->GetEsdfVisualization(points, get_full);
  }
}

bool VoxbloxProcessor::GetEsdfPlaneSlice2D(float plane_height_m, EsdfPlane2D* out,
                                           int max_cells) const {
  return impl_ && impl_->GetEsdfPlaneSlice2D(plane_height_m, out, max_cells);
}

int VoxbloxProcessor::IntegratedFrameCount() const {
  return impl_ ? impl_->integrated_frames : 0;
}

}  // namespace mapping
