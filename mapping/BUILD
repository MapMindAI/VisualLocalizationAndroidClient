package(default_visibility = ["//visibility:public"])


cc_binary(
    name = "mono_vio_ws_stream_client",
    srcs = [
        "mono_vio_ws_stream_client.cc",
    ],
    copts = [
        "-std=c++17",
        "-I/usr/local/include",
    ],
    linkopts = [
        "-lgflags",
        "-lglog",
    ],
    deps = [
        "//mapping/backend:simple_websocket_server",
        "//mapping/common:frame_protocol",
        "//mapping/common:utils",
        "//mapping/da3:da3_onnx_runner",
        "//mapping/da3:render_overlay",
        "//mapping/voxblox:voxblox_processor",
        "@com_github_grpc_grpc//:grpc++",
        "@eigen//:eigen",
        "@opencv//:core",
        "@opencv//:imgproc",
        "@opencv//:highgui",
        "@opencv//:imgcodecs",
        "@sophus//:sophus",
    ],
)

cc_binary(
    name = "dataset_rgbd_player",
    srcs = [
        "dataset_rgbd_player.cc",
    ],
    copts = [
        "-std=c++17",
    ],
    linkopts = [
        "-lgflags",
        "-lglog",
    ],
    deps = [
        "//mapping/common:pose_types",
        "@opencv//:core",
        "@opencv//:imgproc",
        "@opencv//:highgui",
        "@opencv//:imgcodecs",
        "@sophus//:sophus",
    ],
)
