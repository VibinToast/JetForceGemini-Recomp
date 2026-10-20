FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang \
        cmake \
        curl \
        file \
        git \
        libdbus-1-dev \
        libfreetype6-dev \
        libfuse2 \
        libgtk-3-dev \
        libsdl2-dev \
        libvulkan-dev \
        lld \
        mesa-vulkan-drivers \
        ninja-build \
        patchelf \
        python3 \
        squashfs-tools \
        vulkan-tools \
        xdotool \
        xvfb \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
