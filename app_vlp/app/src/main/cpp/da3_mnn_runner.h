#ifndef C_ARCORE_HELLOE_AR_DA3_MNN_RUNNER_H_
#define C_ARCORE_HELLOE_AR_DA3_MNN_RUNNER_H_

#include <android/asset_manager.h>

#include <string>
#include <vector>

namespace hello_ar {

class Da3MnnRunner {
 public:
  explicit Da3MnnRunner(AAssetManager* asset_manager);
  ~Da3MnnRunner();

  Da3MnnRunner(const Da3MnnRunner&) = delete;
  Da3MnnRunner& operator=(const Da3MnnRunner&) = delete;

  bool IsReady() const;

  // Runs DA3 pair inference using two gray frames.
  bool RunPair(const std::vector<uint8_t>& prev_gray, const std::vector<uint8_t>& curr_gray,
               int width, int height, std::string* result_msg);

 private:
  bool Init(std::string* error_msg);
  bool LoadModelFromAssets(std::vector<uint8_t>* model_bytes, std::string* error_msg) const;
  bool LoadModelFromAssets(std::vector<uint8_t>* model_bytes, const char* asset_path,
                           std::string* error_msg) const;
  bool WriteFile(const char* path, const std::vector<uint8_t>& bytes,
                 std::string* error_msg) const;

  AAssetManager* asset_manager_ = nullptr;
  bool initialized_ = false;
  std::string init_error_;
  void* impl_ = nullptr;
};

}  // namespace hello_ar

#endif  // C_ARCORE_HELLOE_AR_DA3_MNN_RUNNER_H_
