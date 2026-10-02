# Linux fallback build + run env (B-019): the SDK's linux CI deps (.github/workflows/_build-platform.yaml in
# rexglue-sdk) on ubuntu 26.04, whose distro clang/cmake are new enough (same base as the Windows cross-build).
#
# Throwaway container, nothing installed on the host. tools/linux/build.sh builds this image as
# `bots-linux` and runs it capped.
# GPU runs need NVIDIA_DRIVER_CAPABILITIES=all + `--gpus all` and the glvnd libs below (else the Vulkan
# loader falls back to llvmpipe, knowledge/hosts.md), and a display: Xvfb here (headless).
FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive \
    NVIDIA_DRIVER_CAPABILITIES=all

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl git python3 python3-venv zip unzip rsync \
      clang lld llvm cmake ninja-build build-essential pkg-config autoconf \
      libgtk-3-dev libx11-xcb-dev libxss-dev libvulkan-dev vulkan-tools \
      libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libdecor-0-dev \
      libasound2-dev libpulse-dev libpipewire-0.3-dev \
      libglvnd0 libgl1 libegl1 libx11-6 libxext6 xvfb xauth \
 && rm -rf /var/lib/apt/lists/*

ENV CC=clang CXX=clang++
