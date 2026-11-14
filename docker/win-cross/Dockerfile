# Windows release build env (tools/ci/build.sh windows): clang + lld cross-compiling to x86_64 MSVC, with the MSVC
# CRT and Windows SDK from xwin in /w/xwin (where tools/win-cross/win-clang.cmake looks). The same toolchain as
# tools/win-cross/build.sh, as one image. Building it accepts the Microsoft license for the CRT and SDK (xwin
# --accept-license); the image is built where it's used (locally, or on the CI runner's Docker host). CI keeps a copy in
# this private project's package registry (tools/ci/ci_image.sh), never anywhere public.
#
# Everything a build downloads is pinned, so rebuilding this unchanged file gives the same toolchain: unpinned, a
# rebuild took whatever SDK Microsoft published that day, and xwin 0.6.6 couldn't read the 10.0.26100 SDK's new
# packaging (2026-11). The pins: the base image by digest, apt packages from Ubuntu's snapshot of one day, xwin and
# rustup-init by version and sha256, and the VS manifest by its versioned URL and sha256 (it names every CRT and SDK
# file with its sha256, which xwin checks). What a pin can't stop is a server deleting a file (Microsoft drops old
# payloads; the snapshot service or GitHub could go away): the build then fails instead of changing, and CI keeps
# using the image it stored in the package registry, which is the backstop for that.
FROM ubuntu:26.04@sha256:065aa8626841f16057b78b027b464cd59c4fac36c204c72dcbf48d66f5fa8a96

ENV DEBIAN_FRONTEND=noninteractive

# Ubuntu's archive as it was at SNAPSHOT (snapshot.ubuntu.com, through apt's APT::Snapshot), so the same package
# versions every time; the setting stays in the image. The snapshot service is https only and the base image has no CA
# certificates: ca-certificates comes in without the TLS check (apt still checks every file against the archive's
# signed index), then everything else with it.
# The second package line: the SDK's Linux dependencies (as docker/linux), because the recompiler that runs codegen is
# built for this machine first, and configuring the SDK for Linux needs them even for that one target.
ARG SNAPSHOT=20261114T000000Z
RUN echo "APT::Snapshot \"$SNAPSHOT\";" > /etc/apt/apt.conf.d/50snapshot \
 && apt-get -o Acquire::https::Verify-Peer=false update \
 && apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends ca-certificates \
 && apt-get update && apt-get install -y --no-install-recommends \
      curl git python3 xz-utils zip clang lld llvm cmake ninja-build pkg-config ccache nsis \
      libgtk-3-dev libx11-xcb-dev libxss-dev libvulkan-dev libwayland-dev libwayland-bin wayland-protocols \
      libxkbcommon-dev libdecor-0-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
 && rm -rf /var/lib/apt/lists/*

# xwin reads the VS channel manifest given here instead of Microsoft's current one (VS 17.14.45, release channel:
# --manifest takes the place of --manifest-version and --channel), and takes the CRT and SDK versions named here
# instead of the newest. The channel manifest names the package manifest, which names every CRT and SDK file with its
# sha256 (xwin checks those). Both manifests are checked here by sha256: xwin can't check the package manifest itself
# (Microsoft's listed hash for it never matches), so it goes into xwin's download cache under the name xwin looks for,
# pkg_manifest_<listed hash>.vsman. They're downloaded at build time and never kept: they and everything they list are
# Microsoft's.
ARG XWIN=0.10.0
ARG XWIN_SHA256=d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88
ARG MS_DL=https://download.visualstudio.microsoft.com/download/pr/95995f57-8355-4ff5-8d90-a7e1669fb3b3
ARG CHMAN=a788dfa3e6f50a33f7dee8396fd40d2946c69a776c0cfa5208f56d461fd1a825/VisualStudio.17.Release.chman
ARG CHMAN_SHA256=44a3e7dc44486c945abdc1fae97e7e22de4d05e80f03b00e9302ba4b72c3e0e3
ARG VSMAN_ID=da49c304df0fff4e75c20849171840a06840e845ce814104d709dfb5cd461668
ARG VSMAN_SHA256=d1d8109899558618035724949673f69f8c1b5704f542a2e76f31d87e425d5710
ARG WIN_SDK=10.0.26100
ARG MSVC_CRT=14.44.17.14
RUN curl -fsSL -o /tmp/xwin.tgz \
      https://github.com/Jake-Shadle/xwin/releases/download/${XWIN}/xwin-${XWIN}-x86_64-unknown-linux-musl.tar.gz \
 && echo "${XWIN_SHA256}  /tmp/xwin.tgz" | sha256sum -c --quiet \
 && tar -xzf /tmp/xwin.tgz -C /tmp && cp /tmp/xwin-*/xwin /usr/local/bin/ && rm -rf /tmp/xwin* \
 && mkdir -p /tmp/xwin-cache/dl \
 && curl -fsSL -o /tmp/vs.chman "$MS_DL/$CHMAN" \
 && echo "${CHMAN_SHA256}  /tmp/vs.chman" | sha256sum -c --quiet \
 && curl -fsSL -o /tmp/xwin-cache/dl/pkg_manifest_${VSMAN_ID}.vsman "$MS_DL/$VSMAN_ID/VisualStudio.vsman" \
 && echo "${VSMAN_SHA256}  /tmp/xwin-cache/dl/pkg_manifest_${VSMAN_ID}.vsman" | sha256sum -c --quiet \
 && xwin --accept-license --cache-dir /tmp/xwin-cache --manifest /tmp/vs.chman \
      --sdk-version ${WIN_SDK} --crt-version ${MSVC_CRT} splat --output /w/xwin \
 && rm -rf /tmp/xwin-cache /tmp/vs.chman \
 && chmod -R a+rwX /w/xwin

ENV CC=clang CXX=clang++

# Rust for n1 (n1/, built by the CMake build with cargo). The version is n1/rust-toolchain.toml's: keep them equal, or
# the first cargo call in every job downloads the toolchain again. rustup's proxies go on PATH through /usr/local/bin.
# rustup-init is pinned by version and sha256; the toolchain by its version, whose files rustup checks against that
# release's manifest.
ARG RUSTUP=1.29.3
ARG RUSTUP_SHA256=b1c8cb14e30889e7e2999e6b69e62d9e9b9ffc0f4df8180564d306d9680e6c55
ARG RUST_VERSION=1.97.1
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo
RUN curl -fsSL -o /tmp/rustup-init \
      https://static.rust-lang.org/rustup/archive/${RUSTUP}/x86_64-unknown-linux-gnu/rustup-init \
 && echo "${RUSTUP_SHA256}  /tmp/rustup-init" | sha256sum -c --quiet && chmod +x /tmp/rustup-init \
 && /tmp/rustup-init -y --no-modify-path --profile minimal --default-toolchain ${RUST_VERSION} -t x86_64-pc-windows-msvc \
 && rm /tmp/rustup-init \
 && for t in cargo rustc rustup; do ln -sf /opt/cargo/bin/$t /usr/local/bin/$t; done \
 && chmod -R a+rwX /opt/rustup /opt/cargo
# Host build scripts and proc macros link with `cc`.
RUN ln -sf /usr/bin/clang /usr/local/bin/cc
