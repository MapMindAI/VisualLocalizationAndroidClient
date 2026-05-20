package(default_visibility = ["//visibility:public"])


cc_binary(
    name = "ws_stream_client",
    srcs = [
        "ws_stream_client.cc",
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
