FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    git \
    ca-certificates \
    qtbase5-dev \
    qtcharts5-dev \
    libqt5sql5-psql \
    libboost-all-dev \
    libpqxx-dev \
    libpq-dev \
    libarrow-dev \
    libparquet-dev \
    python3 \
    python3-pip \
    && pip3 install --no-cache-dir pandas pyarrow matplotlib \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --config Release -j"$(nproc)"

CMD ["./build/telemetry_analyzer", "--help"]
