#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_root=${BUILD_ROOT:-$repo/build/native}
fprintd=${FPRINTD:?Set FPRINTD to the standard fprintd executable}
meson=${MESON:-meson}
export PKG_CONFIG_PATH="$build_root/prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if [[ ! -f "$build_root/source/meson.build" ]]; then
    printf '%s\n' 'Build the native libraries first.' >&2
    exit 1
fi
if [[ ! -e "$build_root/policy/meson-private/coredata.dat" ]]; then
    "$meson" setup "$build_root/policy" "$build_root/source" \
        --buildtype=debugoptimized -Ddrivers=virtual_device \
        -Dintrospection=false -Ddoc=false -Dinstalled-tests=false \
        -Dudev_rules=disabled -Dudev_hwdb=disabled
fi
"$meson" compile -C "$build_root/policy"
/usr/bin/python3 "$repo/tests/test_fprintd_permissions.py" \
    --fprintd "$fprintd" --libdir "$build_root/policy/libfprint" -v
