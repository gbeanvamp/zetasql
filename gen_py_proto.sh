#!/bin/bash

OUTDIR=./python

./bazel-out/host/bin/external/com_google_protobuf/protoc \
	--python_out=$OUTDIR \
	--python_grpc_out=$OUTDIR \
	--proto_path=$OUTDIR \
    -I. \
    -Ibazel-bin \
    -Ibazel-bin/external/com_google_protobuf/ \
    zetasql/resolved_ast/resolved_ast.proto \
    zetasql/resolved_ast/resolved_node_kind.proto \
    zetasql/local_service/*.proto \
    zetasql/proto/*.proto \
    zetasql/public/*.proto \
    zetasql/public/proto/*.proto \
    zetasql/resolved_ast/*.proto 
