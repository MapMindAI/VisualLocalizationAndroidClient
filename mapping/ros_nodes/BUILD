package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "voxblox_ros_node",
    srcs = ["voxblox_ros_node.cc"],
    deps = [
        "//mapping/common:ros2_utils",
        "//mapping/voxblox:voxblox_processor",
        "@opencv//:core",
        "@ros_humble//:ros_humble",
        "@com_github_glog//:glog",
    ],
)

cc_binary(
    name = "pangolin_visualizer_node",
    srcs = ["pangolin_visualizer_node.cc"],
    deps = [
        "//mapping/common:ros2_utils",
        "@opencv//:core",
        "@opencv//:imgproc",
        "@ros_humble//:ros_humble",
        "@pangolin",
        "@com_github_glog//:glog",
        "@sophus",
    ],
)
