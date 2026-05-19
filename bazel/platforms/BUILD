load("@bazel_skylib//lib:selects.bzl", "selects")
load("@bazel_skylib//rules:common_settings.bzl", "string_flag")

package(default_visibility = ["//visibility:public"])

string_flag(
    name = "platform",
    build_setting_default = "linux",
)

config_setting(
    name = "linux",
    flag_values = {
        ":platform": "linux",
    },
)


config_setting(
    name = "linux_x86_64",
    flag_values = {
        ":platform": "linux_x86_64",
    }
)

config_setting(
    name = "linux_arm64",
    flag_values = {
        ":platform": "linux_arm64",
    }
)

config_setting(
    name = "android_arm64",
    flag_values = {
        ":platform": "android_arm64",
    },
)

config_setting(
    name = "android_armv7a",
    flag_values = {
        ":platform": "android_armv7a",
    },
)
