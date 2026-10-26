#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/sbin:/sbin:$PATH"

[[ $(id -u) == 0 ]] || { echo 'Run this test as root inside a disposable VM.' >&2; exit 1; }
systemd-detect-virt --vm >/dev/null || { echo 'Refusing module tests outside a VM.' >&2; exit 1; }
[[ ! -e /dev/sil6250 ]] || { echo 'Refusing tests on a VM with a physical SIL6250 device.' >&2; exit 1; }
[[ $# == 1 ]] || { echo 'Usage: run.sh /path/to/{irq_startup_test,lifetime_test}.ko' >&2; exit 1; }
case $(basename -- "$1") in
    irq_startup_test.ko) module=irq_startup_test ;;
    lifetime_test.ko) module=lifetime_test ;;
    *) echo 'Unknown regression module.' >&2; exit 1 ;;
esac
insmod "$1"
trap 'rmmod "$module"' EXIT
echo "$module passed."
