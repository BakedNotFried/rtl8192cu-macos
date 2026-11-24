#!/bin/sh
set -eu
cd "$(dirname "$0")"
umask 077
binary=station
if [ "${RTL8192CU_LEGACY_CALNET:-0}" = 1 ]; then binary=calnet; fi
out=$(mktemp -d "$PWD/build-$binary.XXXXXX")
mkdir "$out/tmp" "$out/cache"
export TMPDIR="$out/tmp" CLANG_MODULE_CACHE_PATH="$out/cache"
units="probe usb_transport calnet_main calnet_io station/station"
case "${1:-}" in
    --discover-only) join=0; set -- ;;
    "") join=1; units="$units calnet_join lease_probe host_link/client"; set -- -DCALNET_JOIN ;;
    *) exit 2 ;;
esac
if [ "$binary" = calnet ]; then set -- "$@" -DLEGACY_CALNET_DEFAULT; fi
for src in $units; do
    /usr/bin/clang -arch arm64 -std=c11 -Wall -Wextra -Werror -fno-modules -DRTL_BEACON_TOOL "$@" \
        -c "$src.c" -o "$out/${src##*/}.o"
done
/usr/bin/clang++ -arch arm64 -std=c++17 -Wall -Wextra -Werror -fno-modules -c radio/radio.cpp -o "$out/radio.o"
set --
if [ "$join" = 1 ]; then
    /usr/bin/clang -arch arm64 -std=c11 -Wall -Wextra -Werror -fno-modules \
        host_link/helper.c host_link/admin_socket.c -o "$out/host-link-helper"
    /usr/bin/clang -arch arm64 -std=c11 -Wall -Wextra -Werror -fno-modules \
        host_link/verify.c -o "$out/host-link-verify"
    /usr/bin/clang -arch arm64 -Wall -Wextra -Werror -fobjc-arc -fno-modules -c credential.m -o "$out/credential.o"
    sh security/build.sh "$out/wpa" >&2
    set -- "$out/wpa/libcalwpa.a" -framework AppKit -framework Foundation -framework CoreWLAN -framework Security
fi
/usr/bin/clang++ -arch arm64 "$out"/*.o "$@" -framework IOKit -framework CoreFoundation -o "$out/$binary"
printf '%s\n' "$out/$binary"
