#!/usr/bin/env bash
#
# Build and install the SIL6250 fingerprint stack:
#
#   kernel  sil6250.ko broker + udev rule -> DKMS (or plain make)
#   daemon  sil6250d open-fprintd backend -> cargo, systemd unit, D-Bus policy
#
# Stages run in the order above.  Pass stage names to run a subset, e.g.
#
#   ./install.sh kernel        # just the kernel module
#   ./install.sh               # everything
#
# Environment:
#   PREFIX  install prefix for sil6250d binary (default /usr/local)
#   LLVM    forwarded to the kernel Makefile (set LLVM= for a gcc kernel)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

as_root() {
  if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi
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
    make -C "$HERE/kernel" ${LLVM+LLVM="$LLVM"}
    as_root make -C "$HERE/kernel" modules_install
    as_root depmod -a
  fi

  log "Installing udev rule (/dev/sil6250 permissions)"
  as_root install -Dm644 "$HERE/kernel/60-sil6250.rules" \
    /etc/udev/rules.d/60-sil6250.rules
  as_root udevadm control --reload

  log "Loading the module"
  local mperr
  if ! mperr="$(as_root modprobe sil6250 2>&1)"; then
    if printf '%s' "$mperr" | grep -qiE 'key was rejected|required key not available'; then
      warn "modprobe failed: the module signature was rejected under Secure Boot."
      warn "Enroll the DKMS signing key, reboot, then re-run this stage:"
      warn "    sudo mokutil --import /var/lib/dkms/mok.pub"
    elif secure_boot_enabled; then
      warn "modprobe failed under Secure Boot: ${mperr:-unknown error}"
      warn "If this is a signing error, enroll the DKMS key: sudo mokutil --import /var/lib/dkms/mok.pub"
    else
      warn "modprobe failed (no SIL6250 ACPI device on this machine?): ${mperr:-unknown error}"
    fi
  fi
  as_root udevadm trigger -s misc || true
}

stage_daemon() {
  # sil6250d is useless without open-fprintd: it registers the device with the
  # open-fprintd manager over D-Bus. Without it the daemon starts but silently
  # waits forever for the manager, and fprintd-enroll finds no device.
  if ! systemctl cat open-fprintd.service >/dev/null 2>&1; then
    warn "open-fprintd does not appear to be installed (no open-fprintd.service)."
    warn "sil6250d registers with open-fprintd over D-Bus; without it fprintd-enroll"
    warn "will find no device. Install open-fprintd first (see README), then re-run."
  fi

  log "Building sil6250d (open-fprintd Rust backend)"
  cargo build --release --manifest-path "$HERE/Cargo.toml" -p sil6250d

  log "Installing sil6250d binary -> $PREFIX/bin/sil6250d"
  as_root install -Dm755 "$HERE/target/release/sil6250d" "$PREFIX/bin/sil6250d"

  log "Installing D-Bus policy -> /etc/dbus-1/system.d/"
  as_root install -Dm644 "$HERE/sil6250d/io.github.uunicorn.Fprint.conf" \
    /etc/dbus-1/system.d/io.github.uunicorn.Fprint.conf

  # The bus must re-read its config before sil6250d may own its name; without
  # this the daemon's first start fails with "Request to own name refused by
  # policy" until the next dbus reload/reboot.
  log "Reloading D-Bus to apply the new policy"
  as_root systemctl reload dbus 2>/dev/null \
    || as_root systemctl reload dbus-broker 2>/dev/null \
    || warn "could not reload dbus; reboot or 'systemctl reload dbus' before starting sil6250d"

  log "Installing systemd unit -> /etc/systemd/system/sil6250d.service"
  local unit
  unit="$(mktemp)"
  sed "s|/usr/local/bin/sil6250d|$PREFIX/bin/sil6250d|" \
    "$HERE/sil6250d/sil6250d.service" > "$unit"
  as_root install -Dm644 "$unit" /etc/systemd/system/sil6250d.service
  rm -f "$unit"

  as_root systemctl daemon-reload
  as_root systemctl enable sil6250d
  as_root systemctl restart sil6250d || true
}

# True when UEFI Secure Boot is enabled, in which case DKMS must sign the module
# with a MOK key enrolled in the firmware or modprobe will reject it. Best-effort:
# prefer mokutil, fall back to reading the SecureBoot efivar (last byte 1 = on).
secure_boot_enabled() {
  if command -v mokutil >/dev/null 2>&1; then
    mokutil --sb-state 2>/dev/null | grep -qi enabled
    return
  fi
  local var last
  var="$(ls /sys/firmware/efi/efivars/SecureBoot-* 2>/dev/null | head -1)" || return 1
  [ -n "$var" ] || return 1
  last="$(od -An -tu1 "$var" 2>/dev/null | tr -s ' ' '\n' | grep -v '^$' | tail -1)"
  [ "$last" = "1" ]
}


main() {
  local stages=("$@")
  [ ${#stages[@]} -eq 0 ] && stages=(kernel daemon)
  for s in "${stages[@]}"; do
    case "$s" in
      kernel) stage_kernel ;;
      daemon) stage_daemon ;;
      *) die "unknown stage '$s' (kernel|daemon)" ;;
    esac
  done

  # Enrollment only works once the whole stack is present: the kernel module
  # exposes /dev/sil6250, and sil6250d ('daemon') registers the device with
  # open-fprintd over D-Bus. A subset run (e.g. just 'kernel') leaves
  # fprintd-enroll failing with NoSuchDevice, so don't imply it's ready.
  local ran=" ${stages[*]} "
  if [[ "$ran" == *" kernel "* && "$ran" == *" daemon "* ]]; then
    log "Done. Enroll with: fprintd-enroll (or GNOME/KDE Settings)"
  else
    warn "Partial install (ran:${stages[*]})."
    warn "fprintd-enroll needs the full stack — the device only registers after BOTH the"
    warn "'kernel' and 'daemon' stages. Re-run ./install.sh with no arguments for everything."
  fi
}

main "$@"
