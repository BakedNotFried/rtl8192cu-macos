#!/bin/sh
set -eu
cd "$(dirname "$0")"
umask 077
out=$(mktemp -d "$PWD/build-radio.XXXXXX")
mkdir "$out/tmp" "$out/cache"
export TMPDIR="$out/tmp" CLANG_MODULE_CACHE_PATH="$out/cache"
for unit in probe receive usb_transport beacon/beacon; do
    /usr/bin/clang -arch arm64 -std=c11 -Wall -Wextra -Werror -fno-modules \
        -DRTL_BEACON_TOOL -c "$unit.c" -o "$out/${unit##*/}.o"
done
/usr/bin/clang++ -arch arm64 -std=c++17 -Wall -Wextra -fno-modules \
    -c radio/radio.cpp -o "$out/radio.o"
/usr/bin/clang++ -arch arm64 "$out/probe.o" "$out/receive.o" "$out/usb_transport.o" "$out/beacon.o" "$out/radio.o" \
    -framework IOKit -framework CoreFoundation -o "$out/receive"
printf '%s\n' "$out/receive"
