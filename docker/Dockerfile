FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    protobuf-compiler \
    libprotobuf-dev \
    libgrpc++-dev \
    libgrpc-dev

WORKDIR /app

COPY . .

RUN cmake -S . -B build
RUN cmake --build build

CMD ["./build/dscc-node"]