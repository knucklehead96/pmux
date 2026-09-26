#!/usr/bin/env bash
# Build the release binaries into dist/:
#   pmux-<version>-linux-<arch>         dynamically linked, stripped
#   pmux-<version>-linux-<arch>-static  statically linked, stripped
#   SHA256SUMS
# The version comes from project() in CMakeLists.txt (via `pmux --version`).
set -euo pipefail

cd "$(dirname "$0")/.."
jobs=$(nproc 2>/dev/null || echo 4)
arch=$(uname -m)

build() {  # build <dir> <cmake options...>
  local dir=$1
  shift
  cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=Release -DPMUX_STRIP=ON "$@"
  cmake --build "$dir" -j "$jobs"
}

build build-release-dynamic -DPMUX_STATIC=OFF
build build-release-static -DPMUX_STATIC=ON

version=$(build-release-dynamic/pmux --version | awk '{print $2}')
static_version=$(build-release-static/pmux --version | awk '{print $2}')
if [[ -z $version || $version != "$static_version" ]]; then
  echo "release.sh: version mismatch: '$version' vs '$static_version'" >&2
  exit 1
fi

base="pmux-$version-linux-$arch"
rm -rf dist
mkdir -p dist
install -m 0755 build-release-dynamic/pmux "dist/$base"
install -m 0755 build-release-static/pmux "dist/$base-static"
(cd dist && sha256sum "$base" "$base-static" > SHA256SUMS)

for f in "dist/$base" "dist/$base-static"; do
  echo
  echo "== $f"
  file "$f"
  ldd "$f" 2>&1 || true
  echo "size: $(stat -c %s "$f") bytes"
done
echo
cat dist/SHA256SUMS
