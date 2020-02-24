#include <csignal>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_posix.h>
#include "zetasql/local_service/local_service_grpc.h"


namespace zsql = ::zetasql::local_service;

static grpc::Server* GetServer() {
    static grpc::Server* server = []() {
       static zsql::ZetaSqlLocalServiceGrpcImpl* service =
            new zsql::ZetaSqlLocalServiceGrpcImpl();
        grpc::ServerBuilder builder;
        builder.AddListeningPort("unix:///tmp/zetasql.sock", grpc::InsecureServerCredentials());
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

int main() {
    auto server = GetServer();
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server->Wait();
    return 0;
}
