#!/bin/sh
set -eu
cd "$(dirname "$0")"
umask 077
out=$(mktemp -d "$PWD/build-probe.XXXXXX")
mkdir "$out/tmp" "$out/cache"
export TMPDIR="$out/tmp" CLANG_MODULE_CACHE_PATH="$out/cache"
/usr/bin/clang -arch arm64 -std=c11 -Wall -Wextra -Werror -fno-modules \
    probe.c -framework IOKit -framework CoreFoundation -o "$out/probe"
printf '%s\n' "$out/probe"
