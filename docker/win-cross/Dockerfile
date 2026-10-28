# Windows release build env (tools/ci/build.sh windows): clang + lld cross-compiling to x86_64 MSVC, with the MSVC
# CRT and Windows SDK from xwin in /w/xwin (where tools/win-cross/win-clang.cmake looks). The same toolchain as
# tools/win-cross/build.sh, as one image. Building it accepts the Microsoft license for the CRT and SDK (xwin
# --accept-license); the image is built where it's used (locally, or on the CI runner's Docker host), never pushed.
FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive

# The second line: the SDK's Linux dependencies (as docker/linux), because the recompiler that runs codegen is built
# for this machine first, and configuring the SDK for Linux needs them even for that one target.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl git python3 xz-utils zip clang lld llvm cmake ninja-build pkg-config ccache nsis \
      libgtk-3-dev libx11-xcb-dev libxss-dev libvulkan-dev libwayland-dev libwayland-bin wayland-protocols \
      libxkbcommon-dev libdecor-0-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
 && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL -o /tmp/xwin.tgz \
      https://github.com/Jake-Shadle/xwin/releases/download/0.6.6/xwin-0.6.6-x86_64-unknown-linux-musl.tar.gz \
 && echo "61927597fe0234412e2753550916e1d4f67e0ed85a6eee90879f9bfe3d8fc79f  /tmp/xwin.tgz" | sha256sum -c --quiet \
 && tar -xzf /tmp/xwin.tgz -C /tmp && cp /tmp/xwin-*/xwin /usr/local/bin/ && rm -rf /tmp/xwin* \
 && xwin --accept-license splat --output /w/xwin && rm -rf .xwin-cache \
 && chmod -R a+rwX /w/xwin

ENV CC=clang CXX=clang++

# Rust for n1 (n1/, built by the CMake build with cargo). The version is n1/rust-toolchain.toml's: keep them equal, or
# the first cargo call in every job downloads the toolchain again. rustup's proxies go on PATH through /usr/local/bin.
ARG RUST_VERSION=1.97.1
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo
RUN curl -fsSL https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain ${RUST_VERSION} -t x86_64-pc-windows-msvc \
 && for t in cargo rustc rustup; do ln -sf /opt/cargo/bin/$t /usr/local/bin/$t; done \
 && chmod -R a+rwX /opt/rustup /opt/cargo
# Host build scripts and proc macros link with `cc`.
RUN ln -sf /usr/bin/clang /usr/local/bin/cc
