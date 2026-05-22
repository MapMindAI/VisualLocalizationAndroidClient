load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load("@rules_pkg//:pkg.bzl", "pkg_tar")
load("@rules_pkg//:mappings.bzl", "pkg_files")

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

pkg_files(
    name = "mapmind_vlp_api_include",
    prefix = "include",
    srcs = [
        "mapmind_vlp_api.h",
    ],
)

pkg_files(
    name = "mapmind_vlp_api_lib",
    prefix = "lib/" + select({
        "@bazel_platforms//:android_arm64": "arm64-v8a",
        "//conditions:default": "x86_64",
    }),
    srcs = [
        ":libmapmind_vlp_api.so",
    ],
)

pkg_tar(
    name = "mapmind_vlp_api_pkg",
    srcs = [
        ":mapmind_vlp_api_include",
        ":mapmind_vlp_api_lib",
        "@opencv//:opencv_lib",
    ],
    package_dir = "mapmind_vlp_api",
    extension = "tar.gz",
)
