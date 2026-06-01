load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "libmapmind_vlp_api",
    srcs = [
        "mapmind_vlp_api.cc",
    ],
    hdrs = [
        "mapmind_vlp_api.h",
    ],
    deps = [
        "@com_github_grpc_grpc//:grpc++",
        "//mapping/common:utils",
        "@opencv",
    ],
    alwayslink = 1,
)


cc_binary(
    name = "libmapmind_vlp_api.so",
    linkopts = [
        "-lm",
        "-ldl",
        "-Wl,-soname,libmapmind_vlp_api.so",
    ],
    linkshared = 1,
    deps = [
        ":libmapmind_vlp_api",
    ],
)
