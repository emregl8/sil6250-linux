#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/sbin:/sbin:$PATH"

[[ $(id -u) != 0 ]] || { echo 'Run module builds as a regular user.' >&2; exit 1; }
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
logs=${KERNEL_LOG_DIR:-$repo/build/kernel-logs}
mkdir -p -- "$logs"
if [[ $# == 0 ]]; then
    mapfile -t headers < <(python3 - <<'PY'
from pathlib import Path
paths = {p.resolve() for pattern in ['/lib/modules/*/build', '/usr/src/kernels/*']
         for p in Path('/').glob(pattern.lstrip('/')) if p.is_dir()}
for path in sorted(paths):
    if (path / 'Makefile').is_file() and (path / 'include/config/auto.conf').is_file():
        print(path)
PY
    )
else
    headers=("$@")
fi
[[ ${#headers[@]} != 0 ]] || { echo 'No configured kernel headers were found.' >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
for index in "${!headers[@]}"; do
    directory=$(realpath -- "${headers[$index]}")
    module="$work/$index"
    mkdir -- "$module"
    cp -- "$repo/kernel/Makefile" "$repo/kernel/sil6250.c" "$repo/kernel/sil6250_uapi.h" \
        "$repo/kernel/sil6250_irq.h" "$module/"
    make -C "$module" KDIR="$directory" W=1 KCFLAGS=-Werror -j2 2>&1 | tee "$logs/build-$index.log"
    modinfo "$module/sil6250.ko" | tee "$logs/module-$index.log"
    regression="$work/test-$index"
    mkdir -- "$regression"
    cp -- "$repo/tests/kernel/Makefile" "$repo/tests/kernel/irq_startup_test.c" \
        "$repo/tests/kernel/lifetime_test.c" "$regression/"
    make -C "$regression" KDIR="$directory" SIL6250_KERNEL_DIR="$repo/kernel" -j2 \
        2>&1 | tee "$logs/regression-build-$index.log"
done
