#!/bin/sh
#
# Build the release binaries: fully static aarch64 executables, so they run on
# the headset whatever its glibc and libjpeg versions are.
#
# Meant to run inside an arm64 Alpine container.  CI does exactly this; to do
# the same locally (Docker Desktop emulates arm64 on an x86 machine):
#
#     docker run --rm --platform linux/arm64 -v "$PWD:/src" -w /src \
#         alpine:3.20 sh scripts/build-release.sh
#
# Output lands in dist/.

set -eu

apk add --no-cache build-base linux-headers libjpeg-turbo-dev libjpeg-turbo-static >/dev/null

make clean
make static

rm -rf dist
mkdir -p dist

cp framestream dist/framestream-linux-arm64
cp framecap    dist/framecap-linux-arm64
cp install.sh  dist/install.sh

(cd dist && sha256sum * > SHA256SUMS)

file dist/framestream-linux-arm64 2>/dev/null || true
ls -l dist
