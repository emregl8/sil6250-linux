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
mkdir -- "$work/positive" "$work/mutation" "$work/include"
for name in positive mutation; do
    cp -- "$repo/tests/kernel/Makefile" "$repo/tests/kernel/irq_startup_test.c" "$work/$name/"
done
cp -- "$repo/kernel/sil6250_irq.h" "$work/include/"
make -C "$work/positive" KDIR="$headers" SIL6250_KERNEL_DIR="$work/include" -j2 \
    2>&1 | tee "$logs/build.log"
sed -i 's/IRQF_ONESHOT | IRQF_NO_AUTOEN/IRQF_ONESHOT/' "$work/include/sil6250_irq.h"
make -C "$work/mutation" KDIR="$headers" SIL6250_KERNEL_DIR="$work/include" -j2 \
    2>&1 | tee "$logs/mutation-build.log"
root="$work/root"
mkdir -p -- "$root/bin" "$root/dev" "$root/proc" "$root/sys"
cp -- "$busybox" "$root/bin/busybox"
ln -s busybox "$root/bin/sh"
cp -- "$work/positive/irq_startup_test.ko" "$root/positive.ko"
cp -- "$work/mutation/irq_startup_test.ko" "$root/mutation.ko"
cat > "$root/init" <<'INIT'
#!/bin/sh
/bin/busybox --install -s /bin
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
if insmod /positive.ko && rmmod irq_startup_test; then
    if insmod /mutation.ko; then
        rmmod irq_startup_test
    elif dmesg | grep -q 'sil6250_irq_test: failed: -71'; then
        echo SIL6250_IRQ_VM_PASS
        poweroff -f
    fi
fi
echo SIL6250_IRQ_VM_FAIL
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
grep -q '^SIL6250_IRQ_VM_PASS' "$logs/serial.log"
if grep -q 'SIL6250_IRQ_VM_FAIL\|Kernel panic\|BUG:\|WARNING:' "$logs/serial.log"; then
    exit 1
fi
