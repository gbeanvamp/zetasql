#include <csignal>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_posix.h>
#include "zetasql/local_service/local_service_grpc.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"


namespace zsql = ::zetasql::local_service;

ABSL_FLAG(std::string, listen_address, "unix:///tmp/zetasql.sock", "server listen address");

static grpc::Server* GetServer() {
    static grpc::Server* server = []() {
       static zsql::ZetaSqlLocalServiceGrpcImpl* service =
            new zsql::ZetaSqlLocalServiceGrpcImpl();
        grpc::ServerBuilder builder;
        ::std::string listen_address = absl::GetFlag(FLAGS_listen_address);
        builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
        builder.RegisterService(service);
        builder.SetMaxMessageSize(INT_MAX);
        return builder.BuildAndStart().release();
    }();
    return server;
}

void signal_handler(int signal_num) {
    auto server = GetServer();
    server->Shutdown();
    exit(signal_num);
}

int main(int argc, char** argv) {
    absl::ParseCommandLine(argc, argv);

    auto server = GetServer();
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server->Wait();
    return 0;
}
