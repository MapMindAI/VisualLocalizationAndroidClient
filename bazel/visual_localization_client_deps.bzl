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

    maybe(
        http_archive,
        name = "nlohmann_json",
        build_file = "@mm_visual_localization_client//third_party:nlohmann_json.BUILD",
        sha256 = "e5c7a9f49a16814be27e4ed0ee900ecd0092bfb7dbfca65b5a421b774dccaaed",
        url = "https://github.com/nlohmann/json/releases/download/v3.11.2/include.zip",
    )
