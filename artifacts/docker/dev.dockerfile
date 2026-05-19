
################################################################################

FROM ubuntu:22.04 AS builder

RUN rm -f /etc/apt/sources.list.d/*.list || true

# Install tools for building.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    curl \
    git \
    pkg-config \
    software-properties-common \
    unzip \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Install lib dependencies.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libatlas-base-dev \
    libavcodec-dev \
    libavformat-dev \
    libblas-dev \
    libgl1-mesa-dev \
    libgtk-3-dev \
    liblapack-dev \
    libsuitesparse-dev \
    libswscale-dev \
    libvtk9-dev \
    libeigen3-dev \
    libc++-dev \
    && rm -rf /var/lib/apt/lists/*

# Compile all libs with install prefix /tmp/build.
COPY installers/opencv.sh /tmp/installers/
RUN bash /tmp/installers/opencv.sh /tmp/build && rm /tmp/installers/opencv.sh

COPY installers/pangolin.sh /tmp/installers/
RUN bash /tmp/installers/pangolin.sh /tmp/build && rm /tmp/installers/pangolin.sh

################################################################################

FROM ubuntu:22.04

# Set locale.
RUN apt-get update -y && apt-get install -y locales && rm -rf /var/lib/apt/lists/* \
    && localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8
ENV LANG en_US.utf8

# Install tools for installers.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    curl \
    git \
    git-lfs \
    g++ \
    pkg-config \
    software-properties-common \
    unzip \
    wget \
    libcurl4-openssl-dev \
    sshfs \
    sudo \
    vim \
    default-jre=2:1.11* \
    openjdk-17-jdk openjdk-17-jre \
    && rm -rf /var/lib/apt/lists/*

RUN ldconfig

COPY --from=builder /tmp/build /usr/local

# Install Bazel.
COPY installers/bazel.sh /tmp/installers/
RUN bash /tmp/installers/bazel.sh && rm /tmp/installers/bazel.sh

COPY installers/android.sh /tmp/installers/
ENV ANDROID_HOME /opt/android-sdk
ENV ANDROID_NDK_VERSION 25.1.8937393
ENV ANDROID_NDK_HOME $ANDROID_HOME/ndk/$ANDROID_NDK_VERSION
RUN bash /tmp/installers/android.sh && rm /tmp/installers/android.sh
ENV PATH $PATH:$ANDROID_HOME/cmdline-tools/bin:$ANDROID_HOME/platform-tools
