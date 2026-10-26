#!/usr/bin/env bash
set -euo pipefail

[[ $(id -u) != 0 && $# == 2 ]] || {
    echo 'Usage (regular user): irq-vm.sh /path/to/vmlinuz /path/to/kernel/headers' >&2
    exit 1
}
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
kernel_image=$(realpath -- "$1")
headers=$(realpath -- "$2")
busybox=$(command -v "${BUSYBOX:-busybox}")
if readelf -l "$busybox" | grep -q INTERP; then
    echo 'A static BusyBox is required (busybox-static).' >&2
    exit 1
fi
logs=${IRQ_VM_LOG_DIR:-$repo/build/kernel-runtime}
mkdir -p -- "$logs"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -- "$work/positive" "$work/mutation" "$work/include" \
    "$work/close-mutation" "$work/remove-mutation"
for name in positive mutation; do
    cp -- "$repo/tests/kernel/Makefile" "$repo/tests/kernel/irq_startup_test.c" \
        "$repo/tests/kernel/lifetime_test.c" "$work/$name/"
done
cp -- "$repo/kernel/sil6250.c" "$repo/kernel/sil6250_uapi.h" \
    "$repo/kernel/sil6250_irq.h" "$work/include/"
make -C "$work/positive" KDIR="$headers" SIL6250_KERNEL_DIR="$work/include" -j2 \
    2>&1 | tee "$logs/build.log"
sed -i 's/IRQF_ONESHOT | IRQF_NO_AUTOEN/IRQF_ONESHOT/' "$work/include/sil6250_irq.h"
make -C "$work/mutation" KDIR="$headers" SIL6250_KERNEL_DIR="$work/include" -j2 \
    2>&1 | tee "$logs/mutation-build.log"
for name in close-mutation remove-mutation; do
    mkdir -- "$work/$name/include"
    cp -- "$repo/tests/kernel/Makefile" "$repo/tests/kernel/lifetime_test.c" \
        "$repo/tests/kernel/irq_startup_test.c" "$work/$name/"
    cp -- "$repo/kernel/sil6250.c" "$repo/kernel/sil6250_uapi.h" \
        "$repo/kernel/sil6250_irq.h" "$work/$name/include/"
done
python3 - "$work" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
mutations = {
    'close-mutation': (
        '\tmutex_lock(&s->lifecycle_lock);\n'
        '\tif (atomic_dec_and_test(&s->users))\n'
        '\t\twake_up_all(&s->users_wait);\n'
        '\tmutex_unlock(&s->lifecycle_lock);',
        '\tif (atomic_dec_and_test(&s->users))\n'
        '\t\twake_up_all(&s->users_wait);',
    ),
    'remove-mutation': (
        '\twait_event(s->users_wait, atomic_read(&s->users) == 0);\n'
        '\tmutex_lock(&s->lifecycle_lock);\n'
        '\tmutex_unlock(&s->lifecycle_lock);',
        '\twait_event(s->users_wait, atomic_read(&s->users) == 0);',
    ),
}
for name, (old, new) in mutations.items():
    source = root / name / 'include' / 'sil6250.c'
    contents = source.read_text()
    if contents.count(old) != 1:
        raise SystemExit(f'{name}: expected exactly one target')
    source.write_text(contents.replace(old, new))
PY
for name in close-mutation remove-mutation; do
    make -C "$work/$name" KDIR="$headers" \
        SIL6250_KERNEL_DIR="$work/$name/include" -j2 \
        2>&1 | tee "$logs/$name-build.log"
done
root="$work/root"
mkdir -p -- "$root/bin" "$root/dev" "$root/proc" "$root/sys"
cp -- "$busybox" "$root/bin/busybox"
ln -s busybox "$root/bin/sh"
cp -- "$work/positive/irq_startup_test.ko" "$root/positive.ko"
cp -- "$work/mutation/irq_startup_test.ko" "$root/mutation.ko"
cp -- "$work/positive/lifetime_test.ko" "$root/lifetime.ko"
cp -- "$work/close-mutation/lifetime_test.ko" "$root/close-mutation.ko"
cp -- "$work/remove-mutation/lifetime_test.ko" "$root/remove-mutation.ko"
cat > "$root/init" <<'INIT'
#!/bin/sh
/bin/busybox --install -s /bin
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
if ! insmod /positive.ko || ! rmmod irq_startup_test || \
   ! insmod /lifetime.ko || ! rmmod lifetime_test; then
    echo SIL6250_KERNEL_VM_FAIL
    poweroff -f
fi
for module in /mutation.ko /close-mutation.ko /remove-mutation.ko; do
    if ! dmesg -c >/dev/null; then
        echo SIL6250_KERNEL_VM_FAIL
        poweroff -f
    fi
    if insmod "$module"; then
        case "$module" in
            /mutation.ko) rmmod irq_startup_test ;;
            *) rmmod lifetime_test ;;
        esac
        echo SIL6250_KERNEL_VM_FAIL
        poweroff -f
    fi
    case "$module" in
        /mutation.ko) pattern='sil6250_irq_test: failed: -71' ;;
        *) pattern='sil6250_lifetime_test: failed: -71' ;;
    esac
    if ! dmesg | grep -q "$pattern"; then
        echo SIL6250_KERNEL_VM_FAIL
        poweroff -f
    fi
done
echo SIL6250_KERNEL_VM_PASS
poweroff -f
INIT
chmod 0755 "$root/init"
fakeroot bash -c '
    set -euo pipefail
    cd -- "$1"
    mknod dev/console c 5 1
    mknod dev/null c 1 3
    find . -print0 | cpio --null -o --format=newc --quiet | gzip -n
' -- "$root" > "$work/initramfs.gz"
timeout 180 qemu-system-x86_64 -no-user-config -nodefaults \
    -accel tcg,thread=multi -cpu max -smp 2 -m 512 \
    -display none -serial stdio -monitor none -net none -no-reboot \
    -kernel "$kernel_image" -initrd "$work/initramfs.gz" \
    -append 'console=ttyS0 rdinit=/init panic=-1 oops=panic' \
    2>&1 | tee "$logs/serial.log"
grep -q '^SIL6250_KERNEL_VM_PASS' "$logs/serial.log"
if grep -q 'SIL6250_KERNEL_VM_FAIL\|Kernel panic\|BUG:\|WARNING:' "$logs/serial.log"; then
    exit 1
fi
