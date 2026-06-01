package(default_visibility = ["//visibility:public"])

cc_library(
    name = "pose_types",
    hdrs = ["pose_types.h"],
    deps = [
        "@eigen//:eigen",
        "@sophus//:sophus",
    ],
)

cc_library(
    name = "utils",
    srcs = ["utils.cc"],
    hdrs = ["utils.h"],
    copts = [
        "-std=c++17",
    ],
    deps = [
        ":pose_types",
        "@com_github_glog//:glog",
    ],
)

cc_library(
    name = "frame_protocol",
    srcs = ["frame_protocol.cc"],
    hdrs = ["frame_protocol.h"],
    copts = [
        "-std=c++17",
    ],
    deps = [
        ":pose_types",
        ":utils",
    ],
)

cc_library(
    name = "data_session",
    srcs = ["data_session.cc"],
    hdrs = ["data_session.h"],
    copts = [
        "-std=c++17",
    ],
    deps = [
        ":frame_protocol",
        ":utils",
        "@opencv//:core",
    ],
)
