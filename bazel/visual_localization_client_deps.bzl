load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")

def visual_localization_client_deps():
    """Loads dependencies required by visual localization client tools."""
    maybe(
        native.new_local_repository,
        name = "opencv",
        build_file = "@mm_visual_localization_client//third_party:opencv.BUILD",
        path = "/usr/local",
    )

    maybe(
        native.new_local_repository,
        name = "bazel_platforms",
        build_file = "@mm_visual_localization_client//bazel/platforms:BUILD",
        path = ".",
    )

    maybe(
        http_archive,
        name = "com_github_grpc_grpc",
        strip_prefix = "grpc-1.62.2",
        urls = ["https://github.com/grpc/grpc/archive/refs/tags/v1.62.2.tar.gz"],
    )
