#!/usr/bin/env bash
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../.." && pwd)"
if [[ $(id -u) == 0 ]]; then
    printf '%s\n' 'Run this build as your regular user.' >&2
    exit 1
fi

build_root="${BUILD_ROOT:-$repo/build/native}"
mkdir -p -- "$build_root"
build_root="$(cd -- "$build_root" && pwd)"
source_dir="$build_root/source"
prefix="$build_root/prefix"
meson_build="$build_root/meson"
meson="${MESON:-meson}"
revision="$(cat "$here/revision")"
patches=("$here/patches/0001-misc-device-discovery.patch"
         "$here/patches/0002-sil6250-driver.patch"
         "$here/patches/0003-test-callback-signatures.patch")

if [[ ! -e "$source_dir" ]]; then
    git clone --no-checkout https://gitlab.freedesktop.org/libfprint/libfprint.git "$source_dir"
    git -C "$source_dir" checkout --detach "$revision"
fi
if [[ $(git -C "$source_dir" rev-parse HEAD) != "$revision" ]]; then
    printf '%s\n' 'The libfprint checkout does not match the pinned revision.' >&2
    exit 1
fi
if git -C "$source_dir" apply --check "${patches[@]}" 2>/dev/null; then
    git -C "$source_dir" apply "${patches[@]}"
elif ! git -C "$source_dir" apply --reverse --check "${patches[@]}"; then
    printf '%s\n' 'The libfprint patches do not match the existing checkout. Use a fresh BUILD_ROOT after patch updates.' >&2
    exit 1
fi

make -C "$repo/sil6250-ffi" check install PREFIX="$prefix" \
    CARGO_TARGET_DIR="$build_root/rust" CARGO="${CARGO:-cargo}"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
setup_args=(setup "$meson_build" "$source_dir" --prefix="$prefix"
    --libdir=lib --buildtype="${LIBFPRINT_BUILDTYPE:-release}"
    -Ddrivers="${LIBFPRINT_DRIVERS:-default,sil6250}"
    -Dintrospection=false -Ddoc=false -Dinstalled-tests=false
    -Dudev_rules=disabled -Dudev_hwdb=disabled)
if [[ -f "$meson_build/meson-private/coredata.dat" ]]; then
    setup_args+=(--reconfigure)
fi
"$meson" "${setup_args[@]}"
"$meson" compile -C "$meson_build"
"$meson" test -C "$meson_build" --print-errorlogs
"$meson" install -C "$meson_build" --no-rebuild
printf 'Local prefix: %s\n' "$prefix"
