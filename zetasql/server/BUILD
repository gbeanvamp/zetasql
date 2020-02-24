# ZetaSQL Server
package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "server",
    srcs = ["server.cc"],
    deps = [
        "//zetasql/local_service:local_service_grpc",
        "@com_github_grpc_grpc//:grpc++",
    ],
)