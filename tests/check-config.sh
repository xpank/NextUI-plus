#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"${CC:-cc}" -std=gnu99 -O1 -Wall -Wextra -Wno-format-zero-length \
    "$root/tests/test-config.c" -o "$tmp/test-config"
"$tmp/test-config" "$tmp"
