# RS2V Server - Multi-stage Docker build
# Stage 1: Build
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    zlib1g-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy source code
COPY CMakeLists.txt ./
COPY src/ src/
COPY include/ include/
COPY telemetry/ telemetry/
COPY config/ config/
COPY data/ data/

# Build release
RUN mkdir -p build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_TELEMETRY=ON \
        -DENABLE_SCRIPTING=OFF \
        -DENABLE_COMPRESSION=ON \
        -DBUILD_TESTS=OFF && \
    cmake --build . --parallel $(nproc) && \
    cmake --install . --prefix /install

# Stage 2: Runtime
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libssl3 \
    zlib1g \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -s /bin/false -m rs2v

WORKDIR /opt/rs2v

# Copy built binary and configuration
COPY --from=builder /install/bin/rs2v_server ./bin/
COPY --from=builder /install/etc/rs2v/ ./config/
COPY --from=builder /install/share/rs2v/data/ ./data/

# Create required directories
RUN mkdir -p logs telemetry/output protocol_analysis && \
    chown -R rs2v:rs2v /opt/rs2v

# Environment configuration
ENV RS2V_CONFIG_FILE=/opt/rs2v/config/server.ini
ENV RS2V_LOG_DIR=/opt/rs2v/logs
ENV RS2V_TELEMETRY_DIR=/opt/rs2v/telemetry/output

# Expose game port, query port, EAC port, Prometheus metrics
EXPOSE 7777/udp 27015/udp 7957/tcp 9090/tcp

# Health check
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD test -f /opt/rs2v/logs/*.log || exit 1

USER rs2v

ENTRYPOINT ["./bin/rs2v_server"]
CMD ["-c", "config/server.ini"]
