#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

cd "$ROOT_DIR"
make
./validate.sh

printf 'Demo workbook created at %s\n' "$ROOT_DIR/build/validation/tiny.xlsx"
