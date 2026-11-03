#!/bin/bash
# Build and install a server-side XRootD release from source.
#   build-xrootd.sh <git tag, e.g. v6.2.1> <install prefix>
set -euo pipefail
tag=$1
prefix=$2

src=$(mktemp -d)
git clone --quiet --depth 1 --branch "$tag" https://github.com/xrootd/xrootd.git "$src"
cmake -S "$src" -B "$src/build" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_PYTHON=OFF -DENABLE_TESTS=OFF -DENABLE_FUSE=OFF \
  -DENABLE_SCITOKENS=OFF -DENABLE_MACAROONS=OFF -DENABLE_VOMS=OFF -DENABLE_XRDEC=OFF
cmake --build "$src/build" -j"$(nproc)"
cmake --install "$src/build"
rm -rf "$src"
grep '#define XrdVERSION ' "$prefix/include/xrootd/XrdVersion.hh"
