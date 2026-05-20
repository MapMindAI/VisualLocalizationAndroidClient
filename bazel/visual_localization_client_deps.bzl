load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "new_git_repository")

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

    maybe(
        http_archive,
        name = "fmt",
        build_file = "@mm_visual_localization_client//third_party:fmt.BUILD",
        strip_prefix = "fmt-10.2.1",
        urls = ["https://github.com/fmtlib/fmt/archive/refs/tags/10.2.1.tar.gz"],
    )

    maybe(
        http_archive,
        name = "eigen",
        build_file = "@mm_visual_localization_client//third_party:eigen.BUILD",
        sha256 = "8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72",
        strip_prefix = "eigen-3.4.0",
        url = "https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz",
    )

    maybe(
        http_archive,
        name = "sophus",
        build_file = "@mm_visual_localization_client//third_party:sophus.BUILD",
        strip_prefix = "Sophus-1.22.10",
        url = "https://github.com/strasdat/Sophus/archive/refs/tags/1.22.10.zip",
    )

    # for voxblox
    maybe(
        new_git_repository,
        name = "eigen_checks",
        build_file = "@mm_visual_localization_client//third_party:eigen_checks.BUILD",
        commit = "22a6247a3df11bc285d43d1a030f4e874a413997",
        remote = "https://github.com/ethz-asl/eigen_checks",
    )

    maybe(
        new_git_repository,
        name = "minkindr",
        build_file = "@mm_visual_localization_client//third_party:minkindr.BUILD",
        commit = "564f12639a8447d4d3e5e7707851424302941056",
        remote = "https://github.com/ethz-asl/minkindr",
    )

    maybe(
        new_git_repository,
        name = "voxblox",
        build_file = "@mm_visual_localization_client//third_party:voxblox.BUILD",
        commit = "c8066b04075d2fee509de295346b1c0b788c4f38",
        remote = "https://github.com/ethz-asl/voxblox",
    )
