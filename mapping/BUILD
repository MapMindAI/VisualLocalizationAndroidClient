package(default_visibility = ["//visibility:public"])


cc_binary(
    name = "mono_vio_ws_stream_client",
    srcs = [
        "mono_vio_ws_stream_client.cc",
    ],
    deps = [
        "//mapping/common:data_session",
        "//mapping/voxblox:voxblox_processor",
        "@opencv//:core",
        "@opencv//:imgproc",
        "@opencv//:imgcodecs",
        "@com_github_glog//:glog",
        "@ros_humble//:ros_humble",
    ],
)
