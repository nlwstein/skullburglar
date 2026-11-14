# Experimental Android build env (tools/android/build.sh): the NDK cross-compiles the game for arm64-v8a (and
# x86_64, for x86 emulators), and the SDK's build tools package it as an APK with no Gradle: javac + d8 for SDL's Java
# glue, aapt2 for the manifest, zipalign + apksigner; Rust (cargo) for n1.
#
# Throwaway container, nothing installed on the host.
#
# Everything a build downloads is pinned (as docker/win-cross), so rebuilding this unchanged file gives the same
# toolchain: the base image by digest, apt packages from Ubuntu's snapshot of one day, and the NDK, build tools,
# platform and rustup-init by version and sha256. A server deleting a file makes the build fail instead of changing;
# CI then keeps using the image it stored in the package registry (tools/ci/ci_image.sh).
FROM ubuntu:26.04@sha256:065aa8626841f16057b78b027b464cd59c4fac36c204c72dcbf48d66f5fa8a96

ENV DEBIAN_FRONTEND=noninteractive

# Ubuntu's archive as it was at SNAPSHOT (APT::Snapshot, kept in the image). The snapshot service is https only and the
# base image has no CA certificates: ca-certificates comes in without the TLS check (apt still checks every file
# against the archive's signed index), then everything else with it.
ARG SNAPSHOT=20261114T000000Z
RUN echo "APT::Snapshot \"$SNAPSHOT\";" > /etc/apt/apt.conf.d/50snapshot \
 && apt-get -o Acquire::https::Verify-Peer=false update \
 && apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends ca-certificates \
 && apt-get update && apt-get install -y --no-install-recommends \
      curl git python3 zip unzip xz-utils file \
      cmake ninja-build ccache openjdk-21-jdk-headless gcc libc6-dev \
 && rm -rf /var/lib/apt/lists/*

# The NDK, build tools and platform straight from Google's repository by file name and sha256, unzipped where
# sdkmanager would put them (the build only uses the paths below). sdkmanager installed them before, but it resolves a
# package name against Google's current repository: `platforms;android-36` is now revision 2, and a revision 3 would
# have come in unnoticed. Downloading these accepts the Android SDK License, as `sdkmanager --licenses` did.
ARG NDK=29.0.14206865
ARG NDK_ZIP=android-ndk-r29-linux.zip
ARG NDK_SHA256=4abbbcdc842f3d4879206e9695d52709603e52dd68d3c1fff04b3b5e7a308ecf
ARG BUILD_TOOLS=36.1.0
ARG BUILD_TOOLS_ZIP=build-tools_r36.1_linux.zip
ARG BUILD_TOOLS_SHA256=a7b5889e4a79fcf3b0976bef40d401f4240fb1eed891d9d91169da1111e11d78
ARG PLATFORM=android-36
ARG PLATFORM_ZIP=platform-36_r02.zip
ARG PLATFORM_SHA256=37607369a28c5b640b3a7998868d45898ebcb777565a0e85f9acf36f29631d2e
ENV ANDROID_HOME=/opt/android-sdk
RUN set -e; R=https://dl.google.com/android/repository; mkdir -p $ANDROID_HOME/ndk $ANDROID_HOME/build-tools $ANDROID_HOME/platforms; \
    get() { curl -fsSL -o /tmp/p.zip "$R/$1" && echo "$2  /tmp/p.zip" | sha256sum -c --quiet \
      && rm -rf /tmp/p && unzip -q /tmp/p.zip -d /tmp/p && rm /tmp/p.zip && mv /tmp/p/* "$3" && rmdir /tmp/p; }; \
    get $NDK_ZIP $NDK_SHA256 $ANDROID_HOME/ndk/${NDK}; \
    get $BUILD_TOOLS_ZIP $BUILD_TOOLS_SHA256 $ANDROID_HOME/build-tools/${BUILD_TOOLS}; \
    get $PLATFORM_ZIP $PLATFORM_SHA256 $ANDROID_HOME/platforms/${PLATFORM}; \
    chmod -R a+rX $ANDROID_HOME

ENV ANDROID_NDK=$ANDROID_HOME/ndk/${NDK} \
    ANDROID_BUILD_TOOLS=$ANDROID_HOME/build-tools/${BUILD_TOOLS} \
    ANDROID_PLATFORM_JAR=$ANDROID_HOME/platforms/${PLATFORM}/android.jar

# Rust for n1 (n1/, built by the CMake build with cargo) with the Android targets (gcc above links its build scripts). The version is
# n1/rust-toolchain.toml's: keep them equal, or the first cargo call in every job downloads the toolchain again.
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
      --target aarch64-linux-android --target x86_64-linux-android \
 && rm /tmp/rustup-init \
 && for t in cargo rustc rustup; do ln -sf /opt/cargo/bin/$t /usr/local/bin/$t; done \
 && chmod -R a+rwX /opt/rustup /opt/cargo
