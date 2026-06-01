package(default_visibility = ["//visibility:public"])

cc_library(
    name = "eigen_checks",
    srcs = glob([
        "include/eigen-checks/*.h",
        "include/eigen-checks/internal/*.h",
    ]),
    includes = ["include/"],
    deps = [
        "@eigen",
        "@com_github_glog//:glog",
    ],
)
