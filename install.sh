#!/usr/bin/env bash
#
# Build and install the full SIL6250 fingerprint stack:
#
#   lib       libsil6250 (userspace core)      -> meson, installed via pkg-config
#   kernel    sil6250.ko broker + udev rule    -> DKMS (or plain make)
#   fprintd   FP_SIL6250 systemd drop-in        -> auto-binds the virtual device
#   libfprint the FpDevice driver overlay        -> patched upstream checkout
#
# Stages run in the order above.  Pass stage names to run a subset, e.g.
#
#   ./install.sh lib kernel        # just the library and the kernel module
#   ./install.sh                   # everything
#
# Environment:
#   PREFIX        install prefix for libsil6250 + libfprint (default /usr/local)
#   LIBFPRINT_REF git ref of libfprint to build against (default pinned below)
#   LLVM          forwarded to the kernel Makefile (set LLVM= for a gcc kernel)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"
LIBFPRINT_URL="https://gitlab.freedesktop.org/libfprint/libfprint.git"
LIBFPRINT_REF="${LIBFPRINT_REF:-a25f71cf97820c51edc4c32f84686fcdc608d9d1}"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

# Run a command with sudo only when we are not already root.
as_root() {
  if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi
}

stage_lib() {
  log "Building libsil6250 (prefix=$PREFIX)"
  meson setup "$HERE/build" "$HERE" \
    --prefix "$PREFIX" --reconfigure 2>/dev/null \
    || meson setup "$HERE/build" "$HERE" --prefix "$PREFIX"
  meson compile -C "$HERE/build"
  log "Installing libsil6250"
  as_root meson install -C "$HERE/build"
  as_root ldconfig || true
}

stage_kernel() {
  log "Building + installing the sil6250 kernel module"
  if command -v dkms >/dev/null 2>&1; then
    local ver
    ver="$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$HERE/kernel/dkms.conf")"
    as_root rm -rf "/usr/src/sil6250-${ver}"
    as_root cp -r "$HERE/kernel" "/usr/src/sil6250-${ver}"
    as_root dkms add    "sil6250/${ver}"
    as_root dkms build  "sil6250/${ver}"
    as_root dkms install "sil6250/${ver}" --force
  else
    warn "dkms not found; building out-of-tree (won't survive kernel upgrades)"
    make -C "$HERE/kernel"
    as_root make -C "$HERE/kernel" modules_install
    as_root depmod -a
  fi

  log "Installing udev rule (/dev/sil6250 permissions)"
  as_root install -Dm644 "$HERE/kernel/60-sil6250.rules" \
    /etc/udev/rules.d/60-sil6250.rules
  as_root udevadm control --reload

  log "Loading the module"
  as_root modprobe sil6250 || warn "modprobe failed (no SIL6250 ACPI device here?)"
  as_root udevadm trigger -s misc || true
}

stage_fprintd() {
  log "Installing fprintd FP_SIL6250 drop-in (auto-binds the virtual device)"
  # Generate the drop-in so LD_LIBRARY_PATH matches the actual install prefix:
  # the distro fprintd links the system libfprint (no sil6250 driver), so we
  # must point it at our patched build under PREFIX.  Include both lib and lib64
  # so the layout guess in get_libdir() can't strand the loader.
  local conf
  conf="$(mktemp)"
  {
    sed '/^Environment=LD_LIBRARY_PATH=/d' "$HERE/fprint-driver/fprintd-sil6250.conf"
    printf 'Environment=LD_LIBRARY_PATH=%s/%s:%s/lib:%s/lib64\n' \
      "$PREFIX" "$(get_libdir)" "$PREFIX" "$PREFIX"
  } > "$conf"
  as_root install -Dm644 "$conf" \
    /etc/systemd/system/fprintd.service.d/10-sil6250.conf
  rm -f "$conf"
  as_root systemctl daemon-reload
  as_root systemctl try-restart fprintd 2>/dev/null || true
}

stage_libfprint() {
  local src="$HERE/libfprint"
  if [ ! -e "$src/meson.build" ]; then
    log "Fetching libfprint @ ${LIBFPRINT_REF:0:12}"
    if [ -d "$HERE/.git" ] && git -C "$HERE" submodule status libfprint >/dev/null 2>&1; then
      git -C "$HERE" submodule update --init libfprint
    else
      git clone "$LIBFPRINT_URL" "$src"
    fi
  fi
  git -C "$src" fetch --depth 1 origin "$LIBFPRINT_REF" 2>/dev/null || true
  git -C "$src" checkout -q "$LIBFPRINT_REF"

  log "Applying the sil6250 driver overlay"
  install -Dm644 "$HERE/fprint-driver/sil6250.c" \
    "$src/libfprint/drivers/sil6250/sil6250.c"
  # Idempotent: skip if the patch is already in place.
  if git -C "$src" apply --reverse --check "$HERE/fprint-driver/libfprint.patch" 2>/dev/null; then
    warn "overlay patch already applied; skipping"
  else
    git -C "$src" apply "$HERE/fprint-driver/libfprint.patch"
  fi

  log "Configuring + building libfprint with the sil6250 driver"
  # Search both lib and lib64 pkgconfig dirs: get_libdir() can guess "lib64"
  # (e.g. when /usr/lib64 is a symlink) while meson installed the .pc under
  # "lib". Including both makes the lookup robust regardless of distro layout.
  local pcpath="$PREFIX/$(get_libdir)/pkgconfig:$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
  PKG_CONFIG_PATH="$pcpath" \
    meson setup "$src/build" "$src" \
      --prefix "$PREFIX" \
      -Ddrivers=default \
      -Dintrospection=false -Ddoc=false -Dgtk-examples=false \
      --reconfigure 2>/dev/null \
  || PKG_CONFIG_PATH="$pcpath" \
    meson setup "$src/build" "$src" \
      --prefix "$PREFIX" \
      -Ddrivers=default \
      -Dintrospection=false -Ddoc=false -Dgtk-examples=false
  meson compile -C "$src/build"
  log "Installing libfprint"
  as_root meson install -C "$src/build"
  as_root ldconfig || true
}

# Best-effort libdir name (Debian/Ubuntu use a multiarch triplet).
get_libdir() {
  if command -v dpkg-architecture >/dev/null 2>&1; then
    echo "lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
  elif [ -d /usr/lib64 ]; then
    echo "lib64"
  else
    echo "lib"
  fi
}

main() {
  local stages=("$@")
  [ ${#stages[@]} -eq 0 ] && stages=(lib kernel fprintd libfprint)
  for s in "${stages[@]}"; do
    case "$s" in
      lib)       stage_lib ;;
      kernel)    stage_kernel ;;
      fprintd)   stage_fprintd ;;
      libfprint) stage_libfprint ;;
      *) die "unknown stage '$s' (lib|kernel|fprintd|libfprint)" ;;
    esac
  done
  log "Done. Enroll with: fprintd-enroll (or GNOME/KDE Settings)"
}

main "$@"
