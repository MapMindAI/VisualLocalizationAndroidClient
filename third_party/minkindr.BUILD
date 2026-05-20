package(default_visibility = ["//visibility:public"])

cc_library(
    name = "minkindr",
    srcs = glob([
        "minkindr/include/kindr/minimal/*.h",
        "minkindr/include/kindr/minimal/**/*.h",
    ]),
    includes = ["minkindr/include/"],
    deps = [
        "@eigen_checks",
    ],
)
