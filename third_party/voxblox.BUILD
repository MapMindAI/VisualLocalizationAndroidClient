package(default_visibility = ["//visibility:public"])

proto_library(
    name = "voxblox_proto",
    srcs = [
        "voxblox/proto/voxblox/Block.proto",
        "voxblox/proto/voxblox/Layer.proto",
    ],
)

cc_proto_library(
    name = "voxblox_cc_proto",
    deps = [
        ":voxblox_proto",
    ],
)

cc_library(
    name = "voxblox",
    hdrs = glob([
        "voxblox/include/voxblox/**/*.h",
    ]),
    srcs = glob([
        "voxblox/src/**/*.cc",
    ]),
    includes = [
        "voxblox/include/",
        "voxblox/proto/",
    ],
    deps = [
        ":voxblox_cc_proto",
        "@minkindr",
    ],
)
