package(default_visibility = ["//visibility:public"])

cc_library(
    name = "opencv",
    srcs = select({
        "@bazel_platforms//:android_arm64": ["lib/opencv_android/arm64-v8a/libopencv_java4.so"],
        "@bazel_platforms//:android_armv7a": ["lib/opencv_android/armeabi-v7a/libopencv_java4.so"],
        "//conditions:default": glob(["lib/libopencv_*.so*"]),
    }),
    hdrs = glob(["include/opencv4/opencv2/**/*"]),
    includes = ["include/opencv4"],
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
