# Linux fallback build + run env (B-019): the SDK's linux CI deps (.github/workflows/_build-platform.yaml in
# rexglue-sdk) on ubuntu 26.04, whose distro clang/cmake are new enough (same base as the Windows cross-build).
#
# Throwaway container, nothing installed on the host. tools/linux/build.sh builds this image as
# `bots-linux` and runs it capped.
# Pinned like docker/win-cross, so rebuilding this unchanged file gives the same packages: the base image by digest and
# apt packages from Ubuntu's snapshot of one day (a deleted snapshot makes the build fail instead of changing).
# GPU runs need NVIDIA_DRIVER_CAPABILITIES=all + `--gpus all` and the glvnd libs below (else the Vulkan
# loader falls back to llvmpipe, knowledge/hosts.md), and a display: Xvfb here (headless).
FROM ubuntu:26.04@sha256:065aa8626841f16057b78b027b464cd59c4fac36c204c72dcbf48d66f5fa8a96

ENV DEBIAN_FRONTEND=noninteractive \
    NVIDIA_DRIVER_CAPABILITIES=all

# Ubuntu's archive as it was at SNAPSHOT (APT::Snapshot, kept in the image). The snapshot service is https only and the
# base image has no CA certificates: ca-certificates comes in without the TLS check (apt still checks every file
# against the archive's signed index), then everything else with it.
ARG SNAPSHOT=20261114T000000Z
RUN echo "APT::Snapshot \"$SNAPSHOT\";" > /etc/apt/apt.conf.d/50snapshot \
 && apt-get -o Acquire::https::Verify-Peer=false update \
 && apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends ca-certificates \
 && apt-get update && apt-get install -y --no-install-recommends \
      curl git python3 python3-venv zip unzip rsync \
      clang lld llvm cmake ninja-build build-essential pkg-config autoconf \
      libgtk-3-dev libx11-xcb-dev libxss-dev libvulkan-dev vulkan-tools \
      libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libdecor-0-dev \
      libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev \
      libglvnd0 libgl1 libegl1 libx11-6 libxext6 xvfb xauth \
 && rm -rf /var/lib/apt/lists/*

ENV CC=clang CXX=clang++
