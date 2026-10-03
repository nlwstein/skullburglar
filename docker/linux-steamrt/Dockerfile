# Linux release build env (B-078): Steam Runtime 3 "sniper" SDK (Debian 11 base, glibc 2.31), so the binaries run on
# any distro with glibc >= 2.31 and inside Steam's sniper container, with nothing bundled.
#
# Sniper's own clang is 11; clang 21 comes from apt.llvm.org (bullseye). C++23 headers come from sniper's gcc-14,
# which ships only static libraries: libstdc++ is linked statically, libgcc_s is the runtime's (gcc 10).
# tools/linux/build.sh builds this image as `bots-linux-steamrt` when IMAGE=steamrt.
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest

ENV DEBIAN_FRONTEND=noninteractive

# The SDK image already has the dev packages (gtk3, X11/xcb, Xss, Vulkan, Wayland + scanner, xkbcommon, libdecor,
# ALSA, Pulse, PipeWire) and wget/gnupg; only the compiler is added.
RUN wget -qO /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
 && echo "deb http://apt.llvm.org/bullseye/ llvm-toolchain-bullseye-21 main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update && apt-get install -y --no-install-recommends clang-21 lld-21 llvm-21 \
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
