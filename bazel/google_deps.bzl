load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "new_git_repository")

def google_deps():
    maybe(
        http_archive,
        name = "com_github_grpc_grpc",
        sha256 = "1f9cf306a79e9a76ff36f2f0563b72c84ab9f2592372a742a234f360ae733e54",
        strip_prefix = "grpc-1.46.7",
        url = "https://github.com/grpc/grpc/archive/refs/tags/v1.46.7.tar.gz",
    )

    maybe(
        http_archive,
        name = "com_google_protobuf",
        sha256 = "04e1ed9664d1325b43723b6a62a4a41bf6b2b90ac72b5daee288365aad0ea47d",
        strip_prefix = "protobuf-3.20.3",
        urls = [
            "https://github.com/protocolbuffers/protobuf/archive/v3.20.3.zip",
        ],
    )

    maybe(
        http_archive,
        name = "com_google_googletest",
        sha256 = "8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7",
        strip_prefix = "googletest-1.14.0",
        url = "https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz",
    )

    maybe(
        http_archive,
        name = "com_github_gflags_gflags",
        sha256 = "34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf",
        strip_prefix = "gflags-2.2.2",
        url = "https://github.com/gflags/gflags/archive/v2.2.2.tar.gz",
    )

    maybe(
        http_archive,
        name = "com_github_glog",
        patch_args = [
            "-p1",
        ],
        patches = [
            "@mm_visual_localization_client//third_party:glog_prefix.diff",
        ],
        sha256 = "122fb6b712808ef43fbf80f75c52a21c9760683dae470154f02bddfc61135022",
        strip_prefix = "glog-0.6.0",
        url = "https://github.com/google/glog/archive/refs/tags/v0.6.0.zip",
    )
