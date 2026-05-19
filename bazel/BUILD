package(default_visibility = ["//visibility:public"])

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

cc_binary(
    name = "ws_stream_client",
    srcs = [
        "ws_stream_client.cc",
    ],
    copts = [
        "-std=c++17",
    ],
    linkopts = [
        "-lgflags",
        "-lglog",
    ],
    deps = [
        ":da3_onnx_runner",
        "@com_github_grpc_grpc//:grpc++",
        "@opencv//:core",
        "@opencv//:imgproc",
        "@opencv//:highgui",
        "@opencv//:imgcodecs",
    ],
)
