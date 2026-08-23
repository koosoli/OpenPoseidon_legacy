ARG STEAMRT_BASE_IMAGE=registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk:latest
FROM ${STEAMRT_BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf-archive \
    gettext \
    autopoint \
    cargo \
    clang-format \
    curl \
    libltdl-dev \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*

# WGPU 29 requires Rust 1.87.  The SteamRT4 SDK apt repository currently
# supplies 1.85, so install the project's declared minimum with rustup rather
# than silently selecting the older system compiler during CMake configure.
ENV RUSTUP_HOME=/opt/rustup
ENV CARGO_HOME=/opt/cargo
ENV PATH="${CARGO_HOME}/bin:${PATH}"
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.87.0

RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg-seed \
    && /opt/vcpkg-seed/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="${VCPKG_ROOT}:${PATH}"

COPY run-build.sh /usr/local/bin/steamrt-build
RUN chmod +x /usr/local/bin/steamrt-build

WORKDIR /work/ofpr
ENTRYPOINT ["/usr/local/bin/steamrt-build"]
