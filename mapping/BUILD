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
        "//mapping/common:data_session",
        "//mapping/common:utils",
        "//mapping/voxblox:voxblox_processor",
        "@opencv//:core",
        "@opencv//:imgproc",
        "@opencv//:imgcodecs",
    ],
)
