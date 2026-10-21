# The Linux release build machine: Ubuntu 22.04 (glibc 2.35), so the game also runs on everything newer: SteamOS 3,
# current Fedora and Ubuntu 22.04 and later. A game built on a newer system does not run on older ones.
# Used by tools/package_release.sh (Docker or Podman). See RELEASING.md in the owner's Recomp-Docs notes.
FROM ubuntu:22.04

ARG LLVM=18
ARG APPIMAGETOOL=1.9.0
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget gnupg lsb-release software-properties-common \
        git python3 python3-pip file patchelf binutils xz-utils desktop-file-utils \
        g++-12 libstdc++-12-dev \
        libsdl2-dev libvulkan-dev libgtk-3-dev libdbus-1-dev libfreetype6-dev \
    && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
    && bash /tmp/llvm.sh "$LLVM" \
    && apt-get install -y --no-install-recommends "lld-$LLVM" \
    && ln -sf "/usr/bin/clang-$LLVM" /usr/local/bin/clang \
    && ln -sf "/usr/bin/clang++-$LLVM" /usr/local/bin/clang++ \
    && ln -sf "/usr/bin/ld.lld-$LLVM" /usr/local/bin/ld.lld \
    && ln -sf "/usr/bin/lld-$LLVM" /usr/local/bin/lld \
    && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

# Ubuntu 22.04's CMake (3.22) is older than some of the libraries need, so a pinned newer one comes from PyPI instead.
RUN pip3 install --no-cache-dir cmake==3.30.5 ninja==1.11.1.1

# appimagetool, unpacked so it runs without FUSE inside the container. Started through a small script, not a symlink:
# its AppRun finds its files next to the path it was started by.
RUN wget -qO /tmp/appimagetool "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL/appimagetool-x86_64.AppImage" \
    && chmod +x /tmp/appimagetool \
    && cd /opt && /tmp/appimagetool --appimage-extract > /dev/null \
    && mv /opt/squashfs-root /opt/appimagetool \
    && printf '#!/bin/sh\nexec /opt/appimagetool/AppRun "$@"\n' > /usr/local/bin/appimagetool \
    && chmod +x /usr/local/bin/appimagetool \
    && rm /tmp/appimagetool

# The build runs as your own user with the sources mounted from outside, so Git must accept any owner.
RUN git config --system --add safe.directory '*'
