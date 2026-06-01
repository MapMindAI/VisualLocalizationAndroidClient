cc_library(
    name = "pangolin",
    srcs = select({
        "@dm_bazel_platforms//platforms:linux_arm64": glob(["aarch64-linux-gnu/lib/libpango*.so"]),
        "//conditions:default": glob(["lib/libpango*.so"]),
    }),
    hdrs = glob([
        "include/pangolin/*.h",
        "include/pangolin/**/*.h",
        "include/pangolin/**/*.hpp",
        "include/sigslot/*.hpp",
    ]),
    defines = ["HAVE_GLEW"],
    includes = [
        "include",
    ],
    linkopts = [
        "-lGL",
        "-lGLEW",
    ],
    linkstatic = 1,
    visibility = ["//visibility:public"],
)
