// Copyright 2022 DeepMirror Inc. All rights reserved.

#ifndef COMMON_REDIRECT_ANDROID_LOG_H_
#define COMMON_REDIRECT_ANDROID_LOG_H_

#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include "common/android_log.h"

namespace dm {
namespace xr {
static void* LoggingFunction(void*) {
  static constexpr char kLogTag[] = "CPPLOG";
  int pfd_[2];
  // Makes stdout line-buffered
  setvbuf(stdout, 0, _IOLBF, 0);
  // Makes stderr unbuffered
  setvbuf(stderr, 0, _IONBF, 0);

  // Creates the pipe and redirect stdout and stderr.
  pipe(pfd_);
  dup2(pfd_[1], 1);
  dup2(pfd_[1], 2);

  ssize_t read_size;
  char buf[4096];

  __android_log_write(ANDROID_LOG_DEBUG, kLogTag,
                      "Start redirecting stdout and stderr to android logcat.");
  while ((read_size = read(pfd_[0], buf, sizeof buf - 1)) > 0) {
    if (buf[read_size - 1] == '\n') {
      --read_size;
    }

    // Adds null-terminator
    buf[read_size] = 0;
    __android_log_write(ANDROID_LOG_DEBUG, kLogTag, buf);
  }
  __android_log_write(ANDROID_LOG_DEBUG, kLogTag,
                      "STOP redirecting stdout and stderr to android logcat.");

  return 0;
}

// Starts a thread that keeps reading stdout into a buffer and write that buffer
// to android logcat.
static int RunLoggingThread() {
  pthread_t logging_thread_;
  // Spawns the logging thread.
  if (pthread_create(&logging_thread_, 0, LoggingFunction, 0) == -1) {
    return -1;
  }

  pthread_detach(logging_thread_);

  return 0;
}

}  // namespace xr
}  // namespace dm

#endif  // COMMON_REDIRECT_ANDROID_LOG_H_
