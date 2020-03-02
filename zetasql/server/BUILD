# ZetaSQL Server
package(default_visibility = ["//visibility:public"])

load("@io_bazel_rules_docker//cc:image.bzl", "cc_image")

cc_binary(
    name = "server",
    srcs = ["server.cc"],
    deps = [
        "//zetasql/local_service:local_service_grpc",
        "@com_github_grpc_grpc//:grpc++",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
    ],
)

cc_image(
    name = "server_image",
    binary = ":server",
)
