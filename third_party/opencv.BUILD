load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "opencv",
    srcs = select({
        "@bazel_platforms//:linux_arm64": glob(["lib/aarch64-linux-gnu/libopencv_*.so"]),
        "@bazel_platforms//:android_arm64": glob(["lib/opencv_android/arm64-v8a/libopencv_*.so"]),
        "//conditions:default": glob(["lib/libopencv_*.so"]),
    }),
    hdrs = glob(["include/opencv4/opencv2/**/*"]),
    strip_include_prefix = "include/opencv4",
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
