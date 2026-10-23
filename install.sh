#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${BUILD_ROOT:-$HERE/build/native}"
PREFIX="${PREFIX:-/usr/local}"
export BUILD_ROOT
export PATH="$PATH:/usr/sbin:/sbin"

log() { printf '%s\n' "$*"; }
warn() { printf '%s\n' "$*" >&2; }
die() { warn "$*"; exit 1; }
as_root() {
    if [[ $(id -u) == 0 ]]; then "$@"; else sudo "$@"; fi
}

stage_build() {
    "$HERE/integrations/libfprint/build.sh"
}

stage_userspace() (
    local stage_dir="${DESTDIR:-$BUILD_ROOT/stage}"
    local built="$BUILD_ROOT/prefix/lib"
    local library_dir="$PREFIX/lib/sil6250"
    local temporary parent
    [[ "$PREFIX" =~ ^/[A-Za-z0-9_./-]+$ ]] || die 'PREFIX must be a simple absolute path.'
    [[ "/$PREFIX/" != *'/../'* ]] || die 'PREFIX must not contain parent-directory components.'
    [[ $(readlink -m -- "$stage_dir") != / ]] || die 'DESTDIR must not be the filesystem root.'
    [[ $(id -u) != 0 ]] || die 'Stage the userspace files as your regular user.'
    [[ ! -e "$stage_dir" && ! -L "$stage_dir" ]] ||
        die 'DESTDIR already exists; choose a fresh staging directory.'
    [[ -f "$built/libfprint-2.so.2.0.0" && -f "$built/libsil6250_ffi.so.0" ]] ||
        die 'Build the native libraries first: ./install.sh build'
    stage_dir="$(readlink -m -- "$stage_dir")"
    parent="$(dirname -- "$stage_dir")"
    mkdir -p -- "$parent"
    temporary="$(mktemp -d "$parent/.sil6250-stage.XXXXXXXX")"
    trap 'rm -rf -- "$temporary"' EXIT
    install -d "$temporary$library_dir" "$temporary/etc/systemd/system/fprintd.service.d"
    install -m 0755 "$built/libfprint-2.so.2.0.0" "$temporary$library_dir/"
    install -m 0755 "$built/libsil6250_ffi.so.0" "$temporary$library_dir/"
    ln -sfn libfprint-2.so.2.0.0 "$temporary$library_dir/libfprint-2.so.2"
    cat > "$temporary/etc/systemd/system/fprintd.service.d/60-sil6250.conf" <<EOF
[Service]
Environment=LD_LIBRARY_PATH=$library_dir
Environment=SIL6250_PSK_MODE=provision
DeviceAllow=/dev/sil6250 rw
UMask=0077
LimitCORE=0
CoredumpFilter=0x0
EOF
    chmod 0644 "$temporary/etc/systemd/system/fprintd.service.d/60-sil6250.conf"
    mv -T -- "$temporary" "$stage_dir"
    log "Staged standard fprintd libraries and override under $stage_dir"
    log 'No system files, services, PAM settings or enrollment records were changed.'
)

stage_check() {
    systemctl show fprintd.service open-fprintd.service sil6250d.service \
        -p Id -p LoadState -p FragmentPath -p ExecStart -p ActiveState
    if systemctl is-active --quiet sil6250d.service ||
       systemctl is-active --quiet open-fprintd.service; then
        warn 'A legacy service name is active. Inspect its ExecStart before migrating.'
    fi
    [[ -c /dev/sil6250 ]] || warn 'The SIL6250 device node is not present.'
}

stage_kernel() (
  local temporary base_version kernelver
  kernelver=${KERNEL_VERSION:-$(uname -r)}
  [[ "$kernelver" =~ ^[A-Za-z0-9._+-]+$ ]] || die 'Invalid kernel version.'
  temporary=$(mktemp -d)
  trap 'rm -rf -- "$temporary"' EXIT
  local sources=(Makefile dkms.conf sil6250.c sil6250_irq.h sil6250_uapi.h)
  local name
  for name in "${sources[@]}"; do
    cp -- "$HERE/kernel/$name" "$temporary/"
  done
  base_version=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$temporary/dkms.conf")
  [[ "$base_version" =~ ^[0-9][A-Za-z0-9._+-]*$ ]] || die 'Invalid DKMS package version.'
  log "Building + installing the sil6250 kernel module"
  make -C "$temporary" KDIR="/lib/modules/$kernelver/build" ${LLVM+LLVM="$LLVM"}
  if command -v dkms >/dev/null 2>&1; then
    as_root bash "$HERE/integrations/kernel/install-dkms.sh" "$temporary" "$base_version" "$kernelver"
  else
    warn "dkms not found; building out-of-tree (won't survive kernel upgrades)"
    as_root make -C "$temporary" KDIR="/lib/modules/$kernelver/build" modules_install
    as_root depmod -a "$kernelver"
  fi

  log "Installing udev rule (/dev/sil6250 permissions)"
  as_root install -Dm644 "$HERE/kernel/69-sil6250.rules" \
    /etc/udev/rules.d/69-sil6250.rules
  as_root udevadm control --reload

  if [[ "$kernelver" != "$(uname -r)" ]]; then
    log "Installed for kernel $kernelver; the running kernel was not changed."
    return
  fi
  if grep -q '^sil6250 ' /proc/modules; then
    log 'SIL6250 is already loaded; it was not unloaded or replaced in memory.'
    log 'A user-initiated reboot is required to activate changed module code.'
    return
  fi
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
    return 1
  fi
  as_root udevadm trigger -s misc
)

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
    [[ ${#stages[@]} != 0 ]] || stages=(build)
    for stage in "${stages[@]}"; do
        case "$stage" in
            build|kernel|stage|check) ;;
            *) die "Unknown stage '$stage' (build|kernel|stage|check)." ;;
        esac
    done
    for stage in "${stages[@]}"; do
        case "$stage" in
            build) stage_build ;;
            kernel) stage_kernel ;;
            stage) stage_userspace ;;
            check) stage_check ;;
        esac
    done
}

main "$@"
