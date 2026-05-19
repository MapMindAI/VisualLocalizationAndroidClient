package(default_visibility = ["//visibility:public"])

cc_library(
    name = "utils",
    srcs = ["utils.cc"],
    hdrs = ["utils.h"],
    copts = [
        "-std=c++17",
    ],
    deps = [
        ":da3_onnx_runner",
        "@com_github_grpc_grpc//:grpc++",
        "@eigen//:eigen",
        "@sophus//:sophus",
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
        ":da3_onnx_runner",
        ":utils",
        "@eigen//:eigen",
        "@sophus//:sophus",
    ],
)

cc_library(
    name = "render_overlay",
    srcs = ["render_overlay.cc"],
    hdrs = ["render_overlay.h"],
    copts = [
        "-std=c++17",
    ],
    deps = [
        ":frame_protocol",
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
        "-I/usr/local/include/onnxruntime",
    ],
    linkopts = [
        "-L/usr/local/lib",
        "-Wl,-rpath,/usr/local/lib",
        "-lonnxruntime",
    ],
    deps = [
        "@opencv//:core",
        "@opencv//:imgproc",
        "@opencv//:imgcodecs",
    ],
)

cc_library(
    name = "simple_websocket_server",
    srcs = ["simple_websocket_server.cc"],
    hdrs = ["simple_websocket_server.h"],
    copts = [
        "-std=c++17",
    ],
    linkopts = [
        "-lssl",
        "-lcrypto",
    ],
)

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
        ":da3_onnx_runner",
        ":frame_protocol",
        ":render_overlay",
        ":simple_websocket_server",
        ":utils",
        "@com_github_grpc_grpc//:grpc++",
        "@eigen//:eigen",
        "@opencv//:core",
        "@opencv//:imgproc",
        "@opencv//:highgui",
        "@opencv//:imgcodecs",
        "@sophus//:sophus",
    ],
)
