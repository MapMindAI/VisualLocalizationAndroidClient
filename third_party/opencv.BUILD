load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_pkg//:mappings.bzl", "pkg_files")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "opencv",
    srcs = select({
        "@bazel_platforms//:linux_arm64": glob(["lib/aarch64-linux-gnu/libopencv_*.so"]),
        "@bazel_platforms//:android_arm64": glob(["lib/android/arm64-v8a/libopencv_*.so"]),
        "//conditions:default": glob(["lib/libopencv_*.so"]),
    }),
    hdrs = glob(["include/opencv4/opencv2/**/*"]),
    strip_include_prefix = "include/opencv4",
)

pkg_files(
    name = "opencv_lib",
    prefix = "lib/" + select({
        "@bazel_platforms//:android_arm64": "arm64-v8a",
        "//conditions:default": "x86_64",
    }),
    srcs = select({
        "@bazel_platforms//:android_arm64": glob(["lib/android/arm64-v8a/libopencv_*.so"]),
        "//conditions:default": glob(["lib/libopencv_*.so"]),
    }),
)

cc_library(
    name = "core",
    deps = [":opencv"],
)

cc_library(
    name = "imgproc",
    deps = [":opencv"],
)

cc_library(
    name = "imgcodecs",
    deps = [":opencv"],
)

cc_library(
    name = "highgui",
    deps = [":opencv"],
)

cc_library(
    name = "flann",
    deps = [":opencv"],
)

cc_library(
    name = "features2d",
    deps = [":opencv"],
)

cc_library(
    name = "calib3d",
    deps = [":opencv"],
)

cc_library(
    name = "video",
    deps = [":opencv"],
)

cc_library(
    name = "videoio",
    deps = [":opencv"],
)
