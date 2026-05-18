FROM ubuntu:22.04

# Set locale.
RUN apt-get update -y && apt-get install -y locales && rm -rf /var/lib/apt/lists/* \
    && localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8
ENV LANG en_US.utf8

# Install tools for installers.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    g++ \
    software-properties-common \
    zip \
    unzip \
    wget \
    default-jre=2:1.11* \
    openjdk-17-jdk openjdk-17-jre \
    && rm -rf /var/lib/apt/lists/*


COPY installers/android.sh /tmp/installers/
ENV ANDROID_HOME /opt/android-sdk
ENV ANDROID_NDK_VERSION 25.1.8937393
ENV ANDROID_NDK_HOME $ANDROID_HOME/ndk/$ANDROID_NDK_VERSION
RUN bash /tmp/installers/android.sh && rm /tmp/installers/android.sh
ENV PATH $PATH:$ANDROID_HOME/cmdline-tools/bin:$ANDROID_HOME/platform-tools
