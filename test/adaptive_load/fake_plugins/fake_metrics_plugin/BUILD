load(
    "@envoy//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_cc_test_library",
    "envoy_package",
)
load("@envoy_api//bazel:api_build_system.bzl", "api_cc_py_proto_library")

licenses(["notice"])  # Apache 2

envoy_package()

api_cc_py_proto_library(
    name = "fake_metrics_plugin_proto",
    srcs = [
        "fake_metrics_plugin.proto",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//api/client:base",
        "@envoy_api//envoy/config/core/v3:pkg",
    ],
)

envoy_cc_test_library(
    name = "fake_metrics_plugin",
    srcs = [
        "fake_metrics_plugin.cc",
    ],
    hdrs = [
        "fake_metrics_plugin.h",
    ],
    repository = "@envoy",
    deps = [
        ":fake_metrics_plugin_proto_cc_proto",
        "//include/nighthawk/adaptive_load:metrics_plugin",
        "@envoy//source/common/common:assert_lib_with_external_headers",
        "@envoy//source/common/common:statusor_lib_with_external_headers",
        "@envoy//source/common/config:utility_lib_with_external_headers",
        "@envoy//source/common/protobuf:protobuf_with_external_headers",
    ],
)

envoy_cc_test(
    name = "fake_metrics_plugin_test",
    srcs = ["fake_metrics_plugin_test.cc"],
    repository = "@envoy",
    deps = [
        ":fake_metrics_plugin",
        "//source/adaptive_load:plugin_loader",
        "//test/test_common:proto_matchers",
        "@envoy//source/common/protobuf:utility_lib_with_external_headers",
        "@envoy//test/test_common:utility_lib",
    ],
)
