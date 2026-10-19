# Build environment for the native Steam Link client.
# bullseye = glibc 2.31, old enough for MuOS; SDL2 is linked dynamically against the device's own copy.
FROM debian:bullseye
# bullseye is EOL and only lives on archive.debian.org now.
RUN rm -f /etc/apt/sources.list.d/* && printf '%s\n' \
      "deb http://archive.debian.org/debian bullseye main" \
      "deb http://archive.debian.org/debian bullseye-updates main" \
      "deb http://archive.debian.org/debian-security bullseye-security main" > /etc/apt/sources.list && \
    echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99archive
RUN dpkg --add-architecture arm64 && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential pkg-config ca-certificates wget xz-utils file nasm \
      gcc-aarch64-linux-gnu libc6-dev-arm64-cross \
      libsdl2-dev libssl-dev libsdl2-dev:arm64 libssl-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*
ARG FFMPEG_VER=6.1.2
RUN wget -q https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VER}.tar.xz -O /tmp/ff.tar.xz && \
    mkdir /src && tar -xf /tmp/ff.tar.xz -C /src && rm /tmp/ff.tar.xz
ENV FFCOMMON="--disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-postproc \
  --enable-decoder=h264 --enable-decoder=opus --enable-parser=h264 --enable-static --disable-shared --enable-pic"
RUN cd /src/ffmpeg-${FFMPEG_VER} && ./configure --prefix=/opt/ff-x86_64 $FFCOMMON && make -j"$(nproc)" && make install && make distclean
RUN cd /src/ffmpeg-${FFMPEG_VER} && ./configure --prefix=/opt/ff-aarch64 $FFCOMMON \
      --enable-cross-compile --arch=aarch64 --target-os=linux --cross-prefix=aarch64-linux-gnu- && \
    make -j"$(nproc)" && make install && make distclean
WORKDIR /work
