# Builds the dscc-node container used by docker-compose.yml.
# This image compiles the current repo inside Ubuntu and starts the gRPC server.
# It is rebuilt by the end-to-end bench whenever the server code changes.

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    protobuf-compiler \
    protobuf-compiler-grpc \
    libprotobuf-dev \
    libgrpc++-dev \
    libgrpc-dev

WORKDIR /app

COPY . .

RUN rm -rf build
RUN cmake -S . -B build
RUN cmake --build build

CMD ["./build/dscc-node"]
