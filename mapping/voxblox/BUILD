package(default_visibility = ["//visibility:public"])

cc_library(
    name = "voxblox_processor",
    srcs = ["voxblox_processor.cc"],
    hdrs = ["voxblox_processor.h"],
    copts = ["-std=c++17"],
    deps = [
        "//mapping/common:utils",
        "//mapping/common:pose_types",
        "@opencv//:core",
        "@voxblox//:voxblox",
    ],
)
