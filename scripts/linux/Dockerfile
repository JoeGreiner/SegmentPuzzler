ARG ALMALINUX_VERSION=9.8
FROM almalinux:${ALMALINUX_VERSION}

RUN dnf install -y dnf-plugins-core epel-release \
    && if [ "$(rpm -E '%{rhel}')" = 8 ]; then \
         dnf config-manager --set-enabled powertools \
         && dnf install -y openssl3-libs; \
       else \
         dnf config-manager --set-enabled crb; \
       fi \
    && dnf install -y --allowerasing gcc-toolset-12-gcc gcc-toolset-12-gcc-c++ \
       python3.12 python3.12-pip git curl tar gzip xz unzip file jq which \
       binutils patchelf desktop-file-utils \
       mesa-libGL-devel mesa-libEGL-devel mesa-dri-drivers \
       libX11-devel libXext-devel libXcursor-devel libXi-devel libXrandr-devel \
       libXrender-devel libXfixes-devel libSM-devel libICE-devel libxcb-devel \
       xcb-util-cursor xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util-wm \
       libxkbcommon-x11-devel fontconfig-devel freetype-devel \
       dbus-libs openssl-libs wayland-devel \
       xorg-x11-server-Xvfb xorg-x11-xauth dejavu-sans-fonts \
    && dnf clean all

COPY requirements.txt /tmp/build-requirements.txt
RUN python3.12 -m pip install --no-cache-dir --no-deps -r /tmp/build-requirements.txt \
    && python3.12 -m pip check

ENV PATH="/opt/rh/gcc-toolset-12/root/usr/bin:/usr/local/bin:${PATH}"
ENV CC=gcc CXX=g++ CI_ROOT=/work BUILD_PARALLEL=3
WORKDIR /source
