FROM ubuntu:latest AS base
RUN apt-get -y update
RUN apt-get -y install wget pkg-config zip g++ zlib1g-dev unzip python3 git make bash-completion python apt-utils
RUN apt-get -y install  openjdk-8-jdk-headless
RUN apt-get -y install python3-distutils
RUN apt-get install --fix-broken
RUN wget https://github.com/bazelbuild/bazel/releases/download/0.29.1/bazel_0.29.1-linux-x86_64.deb
RUN dpkg -i bazel_0.29.1-linux-x86_64.deb


FROM base as builder
COPY . /workspace
WORKDIR /workspace
RUN bazel build --jobs=5 --local_ram_resources=HOST_RAM*.5 --local_cpu_resources=HOST_CPUS*1 //zetasql/server --verbose_failures
RUN bazel build --jobs=5 //protos:local_service


FROM debian:buster-slim
RUN mkdir -p /opt/zetasql
COPY --from=builder /workspace/bazel-bin/zetasql/server/server /opt/zetasql/server
COPY --from=builder /workspace/bazel-bin/protos /opt/zetasql/protos
EXPOSE 5000
CMD ["/opt/zetasql/server", "--listen_address", "0.0.0.0:5000"]
