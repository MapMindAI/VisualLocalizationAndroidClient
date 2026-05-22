workspace(name = "mm_visual_localization_client")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Override old gtest from transitive deps to a Bazel-6-compatible release.
http_archive(
    name = "com_google_googletest",
    sha256 = "8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7",
    urls = ["https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz"],
    strip_prefix = "googletest-1.14.0",
)

load("//bazel:visual_localization_client_deps.bzl", "visual_localization_client_deps")
visual_localization_client_deps()

load("@com_github_grpc_grpc//bazel:grpc_deps.bzl", "grpc_deps")
grpc_deps()

load("@com_github_grpc_grpc//bazel:grpc_extra_deps.bzl", "grpc_extra_deps")
grpc_extra_deps()

# Android toolchains for --config=android / --config=android64.
android_sdk_repository(name = "androidsdk")
load("@rules_android_ndk//:rules.bzl", "android_ndk_repository")
android_ndk_repository(name = "androidndk")
