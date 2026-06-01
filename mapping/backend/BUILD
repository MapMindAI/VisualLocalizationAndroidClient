package(default_visibility = ["//visibility:public"])

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
    deps = [
        "@com_github_glog//:glog",
    ],
)
