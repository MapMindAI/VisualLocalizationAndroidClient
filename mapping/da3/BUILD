package(default_visibility = ["//visibility:public"])

cc_library(
    name = "render_overlay",
    srcs = ["render_overlay.cc"],
    hdrs = ["render_overlay.h"],
    copts = [
        "-std=c++17",
    ],
    deps = [
        "//mapping/common:frame_protocol",
        "@opencv//:core",
        "@opencv//:imgproc",
    ],
)

cc_library(
    name = "da3_onnx_runner",
    srcs = ["da3_onnx_runner.cc"],
    hdrs = ["da3_onnx_runner.h"],
    copts = [
        "-std=c++17",
    ],
    deps = [
        "//mapping/common:pose_types",
        "@nlohmann_json//:json",
        "@opencv//:core",
        "@opencv//:imgproc",
        "@opencv//:imgcodecs",
    ],
)
