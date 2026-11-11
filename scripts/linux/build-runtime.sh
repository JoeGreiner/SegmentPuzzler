#!/usr/bin/env bash
set -euo pipefail

repository=$(cd "$(dirname "$0")/../.." && pwd)
root=${CI_ROOT:?Set CI_ROOT to an absolute build directory on the host}
[[ "$root" = /* ]] || { echo 'CI_ROOT must be absolute' >&2; exit 2; }
mkdir -p "$root/runtime" "$root/logs"
exec > >(tee "$root/logs/runtime-build.log") 2>&1
build_dir=$(mktemp -d "$root/runtime-build.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT

url=$(jq -er '.runtime_source.url' "$repository/scripts/linux/tools.json")
checksum=$(jq -er '.runtime_source.sha256' "$repository/scripts/linux/tools.json")
version=$(jq -er '.runtime_source.version' "$repository/scripts/linux/tools.json")
curl --fail --location --retry 3 "$url" -o "$build_dir/source.tar.gz"
printf '%s  %s\n' "$checksum" "$build_dir/source.tar.gz" | sha256sum --check
tar -xzf "$build_dir/source.tar.gz" -C "$build_dir" --strip-components=1
patch --batch --fuzz=0 -d "$build_dir" -p1 < "$repository/scripts/linux/runtime-appimagelauncher.patch"
patch_hash=$(sha256sum "$repository/scripts/linux/runtime-appimagelauncher.patch" | cut -c 1-12)
printf '%s-launcher-compat-%s\n' "$version" "$patch_hash" > "$build_dir/src/runtime/version"
mkdir "$build_dir/out"

url=$(jq -er '.squashfs_source.url' "$repository/scripts/linux/tools.json")
checksum=$(jq -er '.squashfs_source.sha256' "$repository/scripts/linux/tools.json")
curl --fail --location --retry 3 "$url" -o "$build_dir/squashfs.tar.gz"
printf '%s  %s\n' "$checksum" "$build_dir/squashfs.tar.gz" | sha256sum --check
mkdir "$build_dir/squashfs"
tar -xzf "$build_dir/squashfs.tar.gz" -C "$build_dir/squashfs" --strip-components=1

# Use upstream's static musl/FUSE build separately from the application toolchain.
image=segmentpuzzler-appimage-runtime
docker build --platform linux/amd64 -t "$image" \
    -f "$build_dir/scripts/docker/Dockerfile" "$build_dir"
docker run --rm -i --platform linux/amd64 --user "$(id -u):$(id -g)" \
    -e "BUILD_PARALLEL=${BUILD_PARALLEL:-4}" \
    --workdir /ws -v "$build_dir:/ws:ro" -v "$root/runtime:/ws/out" \
    "$image" bash -euo pipefail <<'EOF'
bash scripts/build-runtime.sh
# appimagetool's bundled mksquashfs only supports zstd; build a static gzip variant.
cp -R squashfs /tmp/squashfs
make -C /tmp/squashfs/squashfs-tools -j"$BUILD_PARALLEL" mksquashfs \
    GZIP_SUPPORT=1 XZ_SUPPORT=0 LZO_SUPPORT=0 LZ4_SUPPORT=0 ZSTD_SUPPORT=0 \
    COMP_DEFAULT=gzip LDFLAGS=-static
install -m 755 /tmp/squashfs/squashfs-tools/mksquashfs /ws/out/mksquashfs
EOF
test -x "$root/runtime/runtime-x86_64"
"$root/runtime/runtime-x86_64" --appimage-version
sha256sum "$root/runtime/runtime-x86_64" "$root/runtime/mksquashfs"

# Fail before building the application if gzip compression is unavailable.
mkdir "$build_dir/gzip-probe"
printf 'gzip probe\n' > "$build_dir/gzip-probe/content"
"$root/runtime/mksquashfs" "$build_dir/gzip-probe" "$build_dir/gzip-probe.squashfs" \
    -comp gzip -noappend -processors "${BUILD_PARALLEL:-4}"
compression=$(od -An -tu2 -j 20 -N 2 "$build_dir/gzip-probe.squashfs" | tr -d '[:space:]')
[[ "$compression" = 1 ]] || { echo 'Expected gzip compression' >&2; exit 1; }
