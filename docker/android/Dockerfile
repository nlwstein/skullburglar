# Experimental Android build env (tools/android/build.sh): the NDK cross-compiles the game for arm64-v8a (and
# x86_64, for x86 emulators), and the SDK's build tools package it as an APK with no Gradle: javac + d8 for SDL's Java
# glue, aapt2 for the manifest, zipalign + apksigner.
#
# Throwaway container, nothing installed on the host.
FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl git python3 zip unzip xz-utils file \
      cmake ninja-build ccache openjdk-21-jdk-headless \
 && rm -rf /var/lib/apt/lists/*

# Pinned: the command line tools only install the packages below, whose versions are spelled out.
ARG CMDLINE_TOOLS=16111833
ARG NDK=29.0.14206865
ARG BUILD_TOOLS=36.1.0
ARG PLATFORM=android-36
ENV ANDROID_HOME=/opt/android-sdk
RUN mkdir -p $ANDROID_HOME/cmdline-tools \
 && curl -fsSL -o /tmp/clt.zip https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_TOOLS}_latest.zip \
 && unzip -q /tmp/clt.zip -d $ANDROID_HOME/cmdline-tools && mv $ANDROID_HOME/cmdline-tools/cmdline-tools $ANDROID_HOME/cmdline-tools/latest \
 && rm /tmp/clt.zip \
 && yes | $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --licenses >/dev/null \
 && $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --install \
      "ndk;${NDK}" "build-tools;${BUILD_TOOLS}" "platforms;${PLATFORM}" >/dev/null \
 && chmod -R a+rX $ANDROID_HOME

ENV ANDROID_NDK=$ANDROID_HOME/ndk/${NDK} \
    ANDROID_BUILD_TOOLS=$ANDROID_HOME/build-tools/${BUILD_TOOLS} \
    ANDROID_PLATFORM_JAR=$ANDROID_HOME/platforms/${PLATFORM}/android.jar
