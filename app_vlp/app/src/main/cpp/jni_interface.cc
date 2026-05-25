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

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>

#include "hello_ar_application.h"

#ifndef HELLO_AR_ENABLE_MAPMIND_VLP
#define HELLO_AR_ENABLE_MAPMIND_VLP 0
#endif

#if HELLO_AR_ENABLE_MAPMIND_VLP
#include "mapmind_vlp_api.h"
#endif

#define JNI_METHOD(return_type, method_name) \
  JNIEXPORT return_type JNICALL              \
      Java_com_mapmind_JniInterface_##method_name

extern "C" {

namespace {
// maintain a reference to the JVM so we can use it later.
static JavaVM *g_vm = nullptr;
static jobject g_control_activity_ref = nullptr;

inline jlong jptr(hello_ar::HelloArApplication *native_hello_ar_application) {
  return reinterpret_cast<intptr_t>(native_hello_ar_application);
}

inline hello_ar::HelloArApplication *native(jlong ptr) {
  return reinterpret_cast<hello_ar::HelloArApplication *>(ptr);
}

#if HELLO_AR_ENABLE_MAPMIND_VLP
JNIEnv* GetOrAttachJniEnv() {
  if (g_vm == nullptr) return nullptr;
  JNIEnv* env = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
    return env;
  }
  if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
    return nullptr;
  }
  return env;
}

void OnGrpcControlCommand(int cmd, void*) {
  JNIEnv* env = GetOrAttachJniEnv();
  if (env == nullptr || g_control_activity_ref == nullptr) return;
  jclass cls = env->GetObjectClass(g_control_activity_ref);
  if (cls == nullptr) return;
  jmethodID mid = env->GetMethodID(cls, "onNativeGrpcControlCommand", "(I)V");
  if (mid == nullptr) return;
  env->CallVoidMethod(g_control_activity_ref, mid, static_cast<jint>(cmd));
}
#endif

}  // namespace

jint JNI_OnLoad(JavaVM *vm, void *) {
  g_vm = vm;
#if HELLO_AR_ENABLE_MAPMIND_VLP
  dm::xr::RunLoggingThread();
#endif
  return JNI_VERSION_1_6;
}

JNI_METHOD(jlong, createNativeApplication)
(JNIEnv *env, jclass, jobject j_asset_manager) {
  AAssetManager *asset_manager = AAssetManager_fromJava(env, j_asset_manager);
  return jptr(new hello_ar::HelloArApplication(asset_manager));
}

JNI_METHOD(jboolean, isDepthSupported)
(JNIEnv *, jclass, jlong native_application) {
  return native(native_application)->IsDepthSupported();
}

JNI_METHOD(void, onSettingsChange)
(JNIEnv *, jclass, jlong native_application,
 jboolean is_instant_placement_enabled) {
  native(native_application)->OnSettingsChange(is_instant_placement_enabled);
}

JNI_METHOD(void, setRenderEnabled)
(JNIEnv *, jclass, jlong native_application, jboolean enabled) {
  native(native_application)->setRenderEnabled(enabled == JNI_TRUE);
}

JNI_METHOD(void, setSceneContentEnabled)
(JNIEnv *, jclass, jlong native_application, jboolean enabled) {
  native(native_application)->setSceneContentEnabled(enabled == JNI_TRUE);
}

JNI_METHOD(void, setDepthSource)
(JNIEnv *, jclass, jlong native_application, jint depth_source) {
  native(native_application)->setDepthSource(static_cast<int>(depth_source));
}

JNI_METHOD(jboolean, isDa2Ready)
(JNIEnv *, jclass, jlong native_application) {
  return static_cast<jboolean>(
      native(native_application)->isDa2Ready() ? JNI_TRUE : JNI_FALSE);
}

JNI_METHOD(void, destroyNativeApplication)
(JNIEnv *env, jclass, jlong native_application) {
#if HELLO_AR_ENABLE_MAPMIND_VLP
  mapmind::vlp::SetControlCommandCallback(nullptr, nullptr);
  if (g_control_activity_ref != nullptr) {
    env->DeleteGlobalRef(g_control_activity_ref);
    g_control_activity_ref = nullptr;
  }
#endif
  delete native(native_application);
}

JNI_METHOD(void, onPause)
(JNIEnv *, jclass, jlong native_application) {
  native(native_application)->OnPause();
}

JNI_METHOD(void, onResume)
(JNIEnv *env, jclass, jlong native_application, jobject context,
 jobject activity) {
  native(native_application)->OnResume(env, context, activity);
#if HELLO_AR_ENABLE_MAPMIND_VLP
  if (g_control_activity_ref != nullptr) {
    env->DeleteGlobalRef(g_control_activity_ref);
    g_control_activity_ref = nullptr;
  }
  g_control_activity_ref = env->NewGlobalRef(activity);
  mapmind::vlp::SetControlCommandCallback(OnGrpcControlCommand, nullptr);
#endif
}

JNI_METHOD(void, onGlSurfaceCreated)
(JNIEnv *, jclass, jlong native_application) {
  native(native_application)->OnSurfaceCreated();
}

JNI_METHOD(int, onSendImage)
(JNIEnv*, jclass, jlong native_application) { return native(native_application)->PublishImage(); }

JNI_METHOD(jboolean, popDebugMessage)
(JNIEnv*, jclass, jlong native_application) {
  return native(native_application)->popDebugMessage();
}

JNI_METHOD(jstring, getDebugMessage)
(JNIEnv* env, jclass, jlong native_application) {
  std::string cppStr = native(native_application)->getDebugMessage();
  return env->NewStringUTF(cppStr.c_str());
}

JNI_METHOD(int, onStartRec)
(JNIEnv* env, jclass, jstring record_folder) {
  // Convert jstring (Java) to std::string (C++)
  const char* cstr = env->GetStringUTFChars(record_folder, nullptr);
  std::string cppString(cstr);
  env->ReleaseStringUTFChars(record_folder, cstr);

#if HELLO_AR_ENABLE_MAPMIND_VLP
  return mapmind::vlp::StartRecording(cppString);
#else
  (void)cppString;
  return -1;
#endif
}

JNI_METHOD(void, onStopRec)
(JNIEnv* env, jclass) {
#if HELLO_AR_ENABLE_MAPMIND_VLP
  mapmind::vlp::StopRecording();
#else
  (void)env;
#endif
}

JNI_METHOD(void, onDisplayGeometryChanged)
(JNIEnv *, jobject, jlong native_application, int display_rotation, int width,
 int height) {
  native(native_application)
      ->OnDisplayGeometryChanged(display_rotation, width, height);
}

JNI_METHOD(void, onGlSurfaceDrawFrame)
(JNIEnv *, jclass, jlong native_application,
 jboolean depth_color_visualization_enabled, jboolean use_depth_for_occlusion) {
  native(native_application)
      ->OnDrawFrame(depth_color_visualization_enabled, use_depth_for_occlusion);
}

JNI_METHOD(void, onTouched)
(JNIEnv *, jclass, jlong native_application, jfloat x, jfloat y) {
  native(native_application)->OnTouched(x, y);
}

JNI_METHOD(jboolean, hasDetectedPlanes)
(JNIEnv *, jclass, jlong native_application) {
  return static_cast<jboolean>(
      native(native_application)->HasDetectedPlanes() ? JNI_TRUE : JNI_FALSE);
}

JNI_METHOD(jboolean, hasLatestStreamFrame)
(JNIEnv*, jclass, jlong native_application) {
  return static_cast<jboolean>(
      native(native_application)->hasLatestStreamFrame() ? JNI_TRUE : JNI_FALSE);
}

JNI_METHOD(jbyteArray, getLatestGrayImage)
(JNIEnv* env, jclass, jlong native_application) {
  std::vector<uint8_t> gray = native(native_application)->getLatestGrayImage();
  jbyteArray output = env->NewByteArray(static_cast<jsize>(gray.size()));
  if (output != nullptr && !gray.empty()) {
    env->SetByteArrayRegion(
        output, 0, static_cast<jsize>(gray.size()),
        reinterpret_cast<const jbyte*>(gray.data()));
  }
  return output;
}

JNI_METHOD(jbyteArray, getLatestYuvNv21Image)
(JNIEnv* env, jclass, jlong native_application) {
  std::vector<uint8_t> yuv = native(native_application)->getLatestYuvNv21Image();
  jbyteArray output = env->NewByteArray(static_cast<jsize>(yuv.size()));
  if (output != nullptr && !yuv.empty()) {
    env->SetByteArrayRegion(
        output, 0, static_cast<jsize>(yuv.size()),
        reinterpret_cast<const jbyte*>(yuv.data()));
  }
  return output;
}

JNI_METHOD(jlongArray, getLatestStreamDimensionsAndTimestamp)
(JNIEnv* env, jclass, jlong native_application) {
  int64_t timestamp_ns = 0;
  int width = 0;
  int height = 0;
  native(native_application)->getLatestStreamMetadata(
      &timestamp_ns, &width, &height, nullptr, nullptr, nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  jlong values[3] = {timestamp_ns, static_cast<jlong>(width),
                     static_cast<jlong>(height)};
  jlongArray output = env->NewLongArray(3);
  if (output != nullptr) {
    env->SetLongArrayRegion(output, 0, 3, values);
  }
  return output;
}

JNI_METHOD(jfloatArray, getLatestStreamIntrinsics)
(JNIEnv* env, jclass, jlong native_application) {
  float fx = 0.0f;
  float fy = 0.0f;
  float cx = 0.0f;
  float cy = 0.0f;
  native(native_application)->getLatestStreamMetadata(
      nullptr, nullptr, nullptr, &fx, &fy, &cx, &cy, nullptr, nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr);
  jfloat values[4] = {fx, fy, cx, cy};
  jfloatArray output = env->NewFloatArray(4);
  if (output != nullptr) {
    env->SetFloatArrayRegion(output, 0, 4, values);
  }
  return output;
}

JNI_METHOD(jfloatArray, getLatestStreamPose)
(JNIEnv* env, jclass, jlong native_application) {
  float qx = 0.0f;
  float qy = 0.0f;
  float qz = 0.0f;
  float qw = 1.0f;
  float tx = 0.0f;
  float ty = 0.0f;
  float tz = 0.0f;
  native(native_application)->getLatestStreamMetadata(
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &qx, &qy,
      &qz, &qw, &tx, &ty, &tz);
  jfloat values[7] = {qx, qy, qz, qw, tx, ty, tz};
  jfloatArray output = env->NewFloatArray(7);
  if (output != nullptr) {
    env->SetFloatArrayRegion(output, 0, 7, values);
  }
  return output;
}

JNIEnv *GetJniEnv() {
  JNIEnv *env;
  jint result = g_vm->AttachCurrentThread(&env, nullptr);
  return result == JNI_OK ? env : nullptr;
}

jclass FindClass(const char *classname) {
  JNIEnv *env = GetJniEnv();
  return env->FindClass(classname);
}

}  // extern "C"
