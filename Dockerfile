# Base image used by the host-binary build stage. Override with
# `docker build --build-arg HOST_BASE=debian:bookworm ...` for older hosts.
ARG HOST_BASE=debian:trixie

# ---------- Stage 1: build the FUSE binary (used by the runtime image) ----------
FROM gcc:13-bookworm AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libfuse3-dev \
        libsqlite3-dev \
        libssl-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY Makefile ./
COPY src ./src

RUN make

# ---------- Stage 2: build BOTH the FUSE binary and the GUI binary ----------
# Used by `docker build --target export --output ...` to extract host binaries.
# Uses Debian Trixie by default to match modern hosts' libfuse3 soname
# (libfuse3.so.4). For older hosts, build with
# `docker build --build-arg HOST_BASE=debian:bookworm ...`.
FROM ${HOST_BASE} AS build-host

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        libfuse3-dev \
        libsqlite3-dev \
        libssl-dev \
        libgtk-3-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY Makefile ./
COPY src ./src
COPY gui ./gui

RUN make all-host

# ---------- Stage 3: scratch image holding just the host binaries ----------
# Built only when explicitly targeted (`--target export`).
FROM scratch AS export
COPY --from=build-host /app/app /app
COPY --from=build-host /app/dedup-gui /dedup-gui

# ---------- Stage 4 (default): headless runtime image ----------
FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        fuse3 \
        libfuse3-3 \
        libsqlite3-0 \
        libssl3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=build /app/app ./app

RUN mkdir -p /mnt/dedup /data \
    && echo "user_allow_other" >> /etc/fuse.conf

VOLUME ["/data"]

CMD ["./app", "-f", "-s", "-o", "allow_other", "/mnt/dedup"]
