#!/bin/sh
# Compatibility entry point for the original personal trial.
set -eu
RTL8192CU_LEGACY_CALNET=1 exec sh "$(dirname "$0")/build-station.sh" "$@"
