#!/bin/sh
# Pass a new directory under the project for the static library and objects.
set -eu
umask 077
cd "$(dirname "$0")/.."
case "${1-}" in
    "$PWD"/*) out=$1 ;;
    *) echo "Supply a new absolute output directory under the prototype" >&2; exit 2 ;;
esac
mkdir "$out"
mkdir "$out/tmp" "$out/cache"
export TMPDIR="$out/tmp" CLANG_MODULE_CACHE_PATH="$out/cache"
vendor="$PWD/security/vendor/wpa_supplicant-2.12/src"
cc=/usr/bin/clang
set -- -std=gnu11 -Os -arch arm64 -fno-modules -I"$vendor" -I"$vendor/utils" \
    -DCONFIG_NO_STDOUT_DEBUG -DCONFIG_NO_WPA_MSG -DCONFIG_NO_HOSTAPD_LOGGER \
    -DCONFIG_NO_RANDOM_POOL -DCONFIG_NO_TKIP -DCONFIG_NO_RC4 -DCONFIG_SHA256 \
    -DCONFIG_CRYPTO_INTERNAL -DCONFIG_INTERNAL_AES
"$cc" "$@" -Wall -Wextra -Wno-unused-parameter -c security/cal_wpa.c -o "$out/cal_wpa.o"
for unit in rsn_supp/wpa rsn_supp/wpa_ie rsn_supp/pmksa_cache \
    common/wpa_common common/ieee802_11_common \
    utils/common utils/wpabuf utils/eloop utils/os_unix utils/wpa_debug \
    crypto/aes-internal crypto/aes-internal-dec crypto/aes-internal-enc \
    crypto/aes-unwrap crypto/aes-wrap crypto/aes-omac1 crypto/sha1 crypto/sha1-internal \
    crypto/sha1-prf crypto/sha256 crypto/sha256-internal crypto/sha256-prf \
    crypto/md5 crypto/md5-internal; do
    "$cc" "$@" -c "$vendor/$unit.c" -o "$out/${unit##*/}.o"
done
/usr/bin/ar -rcs "$out/libcalwpa.a" "$out"/*.o
if [ "${CAL_WPA_TEST:-0}" = 1 ]; then
    "$cc" "$@" -DCONFIG_NO_RADIUS -DCONFIG_NO_VLAN security/test_wpa.c \
        "$vendor/ap/wpa_auth.c" "$vendor/ap/wpa_auth_ie.c" "$vendor/ap/pmksa_cache_auth.c" \
        "$out/libcalwpa.a" -o "$out/test_wpa"
    "$out/test_wpa"
fi
printf '%s\n' "$out/libcalwpa.a"
