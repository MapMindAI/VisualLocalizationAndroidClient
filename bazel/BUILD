cc_binary(
    name = "ws_stream_client",
    srcs = [
        "da3_onnx_runner.h",
        "da3_onnx_runner.cc",
        "ws_stream_client.cc",
    ],
    copts = [
        "-std=c++17",
        "-I/usr/local/include/onnxruntime",
    ],
    linkopts = [
        "-L/usr/local/lib",
        "-Wl,-rpath,/usr/local/lib",
        "-lgflags",
        "-lglog",
        "-lonnxruntime",
    ],
    deps = [
        "@com_github_grpc_grpc//:grpc++",
        "@opencv//:core",
        "@opencv//:imgproc",
        "@opencv//:highgui",
        "@opencv//:imgcodecs",
    ],
)
