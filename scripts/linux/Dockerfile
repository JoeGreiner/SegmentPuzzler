FROM almalinux:8.10

RUN dnf install -y dnf-plugins-core epel-release \
    && dnf config-manager --set-enabled powertools \
    && dnf install -y gcc-toolset-12-gcc gcc-toolset-12-gcc-c++ \
       python3.12 python3.12-pip git curl tar gzip xz unzip file jq which \
       binutils patchelf desktop-file-utils \
       mesa-libGL-devel mesa-libEGL-devel mesa-dri-drivers \
       libX11-devel libXext-devel libXcursor-devel libXi-devel libXrandr-devel \
       libXrender-devel libXfixes-devel libSM-devel libICE-devel libxcb-devel \
       xcb-util-cursor xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util-wm \
       libxkbcommon-x11-devel fontconfig-devel freetype-devel \
       dbus-libs openssl-libs openssl3-libs wayland-devel \
       xorg-x11-server-Xvfb xorg-x11-xauth dejavu-sans-fonts \
    && python3.12 -m pip install --no-cache-dir \
       cmake==3.31.8 ninja==1.11.1.4 aqtinstall==3.3.0 py7zr==1.1.0 \
    && dnf clean all

ENV PATH="/opt/rh/gcc-toolset-12/root/usr/bin:/usr/local/bin:${PATH}"
ENV CC=gcc CXX=g++ CI_ROOT=/work BUILD_PARALLEL=3
WORKDIR /source
