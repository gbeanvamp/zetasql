#!/bin/bash

OUTDIR=./python

./bazel-out/host/bin/external/com_google_protobuf/protoc \
	--python_out=$OUTDIR \
	--python_grpc_out=$OUTDIR \
	--proto_path=$OUTDIR \
    -Ibazel-bin/protos \
    bazel-bin/protos/zetasql/local_service/*.proto \
    bazel-bin/protos/zetasql/proto/*.proto \
    bazel-bin/protos/zetasql/public/*.proto \
    bazel-bin/protos/zetasql/public/proto/*.proto \
    bazel-bin/protos/zetasql/resolved_ast/*.proto
