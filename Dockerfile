# Multi-stage build: the build stage carries the full toolchain, the
# runtime stage carries only the binaries and the shared libraries they
# actually link against -- nobody running the image needs cmake or a
# compiler installed.

FROM ubuntu:24.04 AS build

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential cmake ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
    cmake --build build -j && \
    ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim AS runtime

RUN apt-get update && \
    apt-get install -y --no-install-recommends libstdc++6 && \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/rtp_sender /src/build/rtp_receiver /src/build/impair \
                  /src/build/gen_test_tone /src/build/snr_harness /src/build/wav_snr \
                  /usr/local/bin/

WORKDIR /data
ENTRYPOINT ["/usr/local/bin/rtp_receiver"]
