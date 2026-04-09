// Copyright 2022 DeepMirror Inc. All rights reserved.

#ifndef COMMON_ANDROID_LOG_H_
#define COMMON_ANDROID_LOG_H_

#include <android/log.h>

#define LOG_TAG "MobiliHUD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define ASSERT(cond, fmt, ...)                                \
  if (!(cond)) {                                              \
    __android_log_assert(#cond, LOG_TAG, fmt, ##__VA_ARGS__); \
  }

inline int64_t GetBootTime() {
  struct timespec sys_ts;
  clock_gettime(CLOCK_BOOTTIME, &sys_ts);
  return sys_ts.tv_sec * 1e9 + sys_ts.tv_nsec;
}

#endif  // COMMON_ANDROID_LOG_H_
