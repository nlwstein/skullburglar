# Linux release build env (B-078): Steam Runtime 3 "sniper" SDK (Debian 11 base, glibc 2.31), so the binaries run on
# any distro with glibc >= 2.31 and inside Steam's sniper container, with nothing bundled.
#
# Sniper's own clang is 11; clang 21 comes from apt.llvm.org (bullseye). C++23 headers come from sniper's gcc-14,
# which ships only static libraries: libstdc++ is linked statically, libgcc_s is the runtime's (gcc 10).
# tools/linux/build.sh builds this image as `bots-linux-steamrt` when IMAGE=steamrt.
#
# Everything a build downloads is pinned (as docker/win-cross), so rebuilding this unchanged file gives the same
# toolchain: the SDK image by its build's tag and digest (`latest` moves with every sniper release), apt.llvm.org's
# signing key by sha256 and its packages by exact version, and rustup-init by version and sha256. apt.llvm.org keeps
# only the newest build of each package and has no snapshots: when it replaces 21.1.5 the build fails instead of
# changing, and CI keeps using the image it stored in the package registry (tools/ci/ci_image.sh) until the version
# here is moved on.
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk:3.0.20260805.254768@sha256:1c33c507bc75d012e77df5727f93b0d5b8c3f7c8d4142ba5f7a16882cc92e014

ENV DEBIAN_FRONTEND=noninteractive

# The SDK image already has the dev packages (gtk3, X11/xcb, Xss, Vulkan, Wayland + scanner, xkbcommon, libdecor,
# ALSA, Pulse, PipeWire), wget/gnupg and what the release scripts use (tools/ci/: curl, git, xz-utils, ccache); added
# is the compiler. Only apt.llvm.org packages are installed (clang-21's dependencies are its own, at the same version).
ARG LLVM=1:21.1.5~++20251023083244+45afac62e373-1~exp1~20251023083401.50
RUN wget -qO /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
 && echo "8b2a587ffd672c4687e7581dad4b2f6c1bb2ad6b480cd9771ba2ff48e0b8c75d  /etc/apt/trusted.gpg.d/apt.llvm.org.asc" | sha256sum -c --quiet \
 && echo "deb https://apt.llvm.org/bullseye/ llvm-toolchain-bullseye-21 main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update && apt-get install -y --no-install-recommends clang-21=$LLVM lld-21=$LLVM llvm-21=$LLVM \
 && rm -rf /var/lib/apt/lists/*

# clang/clang++ wrappers: gcc-14's headers and static libstdc++ first, then gcc-10's dir for libgcc_s/libgcc_eh.
RUN G=/usr/lib/gcc-14/lib/gcc/x86_64-linux-gnu/14 \
 && for t in clang clang++; do \
      printf '#!/bin/sh\nexec /usr/bin/%s-21 --gcc-install-dir=%s -L%s -L/usr/lib/gcc/x86_64-linux-gnu/10 -static-libstdc++ -fuse-ld=lld -pthread -Wno-unused-command-line-argument "$@"\n' \
        "$t" "$G" "$G" > /usr/local/bin/$t && chmod +x /usr/local/bin/$t; \
    done \
 && ln -sf /usr/bin/ld.lld-21 /usr/local/bin/ld.lld \
 && for t in llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-strip; do ln -sf /usr/bin/$t-21 /usr/local/bin/$t; done

ENV CC=/usr/local/bin/clang CXX=/usr/local/bin/clang++

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
 && /tmp/rustup-init -y --no-modify-path --profile minimal --default-toolchain ${RUST_VERSION} \
 && rm /tmp/rustup-init \
 && for t in cargo rustc rustup; do ln -sf /opt/cargo/bin/$t /usr/local/bin/$t; done \
 && chmod -R a+rwX /opt/rustup /opt/cargo
