package(default_visibility = ["//visibility:public"])

cc_library(
    name = "ros_humble",
    hdrs = glob([
        "include/*.h",
        "include/**/*.h",
    ]),
    srcs = glob([
        "lib/*.so",
    ]),
    includes = [
        "include/",
        "include/rclcpp",
        "include/rcl",
        "include/rcutils",
        "include/rcpputils",
        "include/rmw",
        "include/rcl_yaml_param_parser",
        "include/rosidl_runtime_c",
        "include/rosidl_typesupport_interface",
        "include/builtin_interfaces",
        "include/rosidl_runtime_cpp",
        "include/tracetools",
        "include/statistics_msgs",
        "include/rcl_interfaces",
        "include/rosbag2_cpp",
        "include/rosbag2_storage",
        "include/libstatistics_collector",
        "include/sensor_msgs",
        "include/std_msgs",
        "include/geometry_msgs",
        "include/message_filters",
        "include/rosidl_typesupport_introspection_cpp",
        "include/tf2_msgs",
    ],
)
