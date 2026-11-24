#!/bin/sh
set -eu
cd "$(dirname "$0")"
umask 077
out=$(mktemp -d "$PWD/build-tests.XXXXXX")
mkdir "$out/tmp" "$out/cache"
export TMPDIR="$out/tmp" CLANG_MODULE_CACHE_PATH="$out/cache"
/usr/bin/clang -std=c11 -Wall -Wextra -Werror -fno-modules -fsanitize=address,undefined \
    beacon/beacon.c beacon/test_beacon.c -o "$out/beacon-test"
/usr/bin/clang++ -std=c++17 -Wall -Wextra -Werror -fno-modules \
    radio/test_rf.cpp -o "$out/rf-test"
/usr/bin/clang++ -std=c++17 -Wall -Wextra -Werror -fno-modules \
    radio/radio.cpp radio/test_firmware.cpp -o "$out/firmware-test"
"$out/beacon-test"
"$out/rf-test"
"$out/firmware-test"
/usr/bin/clang -std=c11 -Wall -Wextra -Werror -fno-modules -fsanitize=address,undefined \
    station/station.c station/test_station.c -o "$out/station-test"
"$out/station-test"
/usr/bin/clang -std=c11 -Wall -Wextra -Werror -fno-modules -fsanitize=address,undefined \
    calnet_join.c test_station_link.c -o "$out/station-link-test"
"$out/station-link-test"
for component in helper client; do
    /usr/bin/clang -std=c11 -Wall -Wextra -Werror -fno-modules -fsanitize=address,undefined \
        "host_link/test_$component.c" host_link/admin_socket.c -o "$out/$component-test"
    "$out/$component-test"
done
/usr/bin/clang -std=c11 -Wall -Wextra -Werror -fno-modules -fsanitize=address,undefined \
    host_link/test_verify.c -o "$out/verify-test"
"$out/verify-test"
/usr/bin/clang -std=c11 -Wall -Wextra -Werror -fno-modules -fsanitize=address,undefined \
    lease_probe.c test_lease_probe.c -o "$out/lease-test"
"$out/lease-test"
CAL_WPA_TEST=1 sh security/build.sh "$out/wpa"
printf 'Offline tests passed in %s\n' "$out"
