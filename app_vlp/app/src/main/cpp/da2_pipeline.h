/*
 * Copyright 2026
 */

#ifndef C_ARCORE_HELLO_AR_DA2_PIPELINE_H_
#define C_ARCORE_HELLO_AR_DA2_PIPELINE_H_

#include <android/asset_manager.h>
#include <stdint.h>

#include <memory>
#include <vector>

namespace hello_ar {

class Da2Pipeline {
 public:
  struct DepthPreview {
    std::vector<uint8_t> rgb;
    int width = 0;
    int height = 0;
    int64_t timestamp_ns = 0;
  };

  explicit Da2Pipeline(AAssetManager* asset_manager);
  ~Da2Pipeline();

  void EnqueueFrame(const std::vector<uint8_t>& gray, int width, int height,
                    int64_t timestamp_ns);
  bool GetLatestDepthPreview(DepthPreview* out) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hello_ar

#endif  // C_ARCORE_HELLO_AR_DA2_PIPELINE_H_
