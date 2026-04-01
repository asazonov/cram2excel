#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
EXTERNAL_DIR="$ROOT_DIR/external"
HTSLIB_VERSION=${HTSLIB_VERSION:-1.23}
SAMTOOLS_VERSION=${SAMTOOLS_VERSION:-1.23}
HTSLIB_DIR="$EXTERNAL_DIR/htslib-$HTSLIB_VERSION"
SAMTOOLS_DIR="$EXTERNAL_DIR/samtools-$SAMTOOLS_VERSION"

mkdir -p "$EXTERNAL_DIR"

if [ ! -d "$HTSLIB_DIR" ]; then
  curl -L -o "$EXTERNAL_DIR/htslib-$HTSLIB_VERSION.tar.bz2" \
    "https://github.com/samtools/htslib/releases/download/$HTSLIB_VERSION/htslib-$HTSLIB_VERSION.tar.bz2"
  tar -xjf "$EXTERNAL_DIR/htslib-$HTSLIB_VERSION.tar.bz2" -C "$EXTERNAL_DIR"
fi

if [ ! -d "$SAMTOOLS_DIR" ]; then
  curl -L -o "$EXTERNAL_DIR/samtools-$SAMTOOLS_VERSION.tar.bz2" \
    "https://github.com/samtools/samtools/releases/download/$SAMTOOLS_VERSION/samtools-$SAMTOOLS_VERSION.tar.bz2"
  tar -xjf "$EXTERNAL_DIR/samtools-$SAMTOOLS_VERSION.tar.bz2" -C "$EXTERNAL_DIR"
fi

cd "$SAMTOOLS_DIR"
./configure --enable-plugins --without-curses --with-plugin-path="$ROOT_DIR/build"
make -j4 all all-htslib
