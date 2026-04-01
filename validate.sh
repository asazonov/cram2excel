#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT_DIR="$SCRIPT_DIR"
SAMTOOLS_DIR=${SAMTOOLS_DIR:-"$ROOT_DIR/external/samtools-1.23"}
BUILD_DIR="$ROOT_DIR/build"
VALIDATION_DIR="$BUILD_DIR/validation"

case "$(uname -s)" in
  Darwin) SO_EXT=bundle ;;
  *) SO_EXT=so ;;
esac

SAMTOOLS_BIN="$SAMTOOLS_DIR/samtools"
PLUGIN_PATH="$BUILD_DIR/hfile_cram2excel.$SO_EXT"

if [ ! -x "$SAMTOOLS_BIN" ]; then
  echo "samtools binary not found at $SAMTOOLS_BIN" >&2
  echo "Run 'make toolchain' first." >&2
  exit 1
fi

if [ ! -f "$PLUGIN_PATH" ]; then
  make -C "$ROOT_DIR"
fi

mkdir -p "$VALIDATION_DIR"
cp "$ROOT_DIR/examples/tiny/tiny.sam" "$VALIDATION_DIR/tiny.sam"
cp "$ROOT_DIR/examples/tiny/tiny_ref.fa" "$VALIDATION_DIR/tiny_ref.fa"

"$SAMTOOLS_BIN" faidx "$VALIDATION_DIR/tiny_ref.fa"
"$SAMTOOLS_BIN" view -C -T "$VALIDATION_DIR/tiny_ref.fa" -o "$VALIDATION_DIR/tiny.cram" "$VALIDATION_DIR/tiny.sam"

HTS_PATH="$BUILD_DIR" "$SAMTOOLS_BIN" view -h "$VALIDATION_DIR/tiny.cram" -o "cram2excel:$VALIDATION_DIR/tiny.xlsx"

unzip -l "$VALIDATION_DIR/tiny.xlsx" >/dev/null
unzip -p "$VALIDATION_DIR/tiny.xlsx" xl/worksheets/sheet1.xml | grep -q "r001"
unzip -p "$VALIDATION_DIR/tiny.xlsx" xl/worksheets/sheet1.xml | grep -q "NM:i:0"
unzip -p "$VALIDATION_DIR/tiny.xlsx" xl/worksheets/sheet1.xml | grep -q "MD:Z:10"
unzip -p "$VALIDATION_DIR/tiny.xlsx" xl/worksheets/sheet2.xml | grep -q "@SQ"

printf 'Validation succeeded: %s\n' "$VALIDATION_DIR/tiny.xlsx"
