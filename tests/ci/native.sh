#!/usr/bin/env bash
set -euo pipefail

[[ $(id -u) != 0 ]] || { echo 'Run native tests as a regular user.' >&2; exit 1; }
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd -- "$repo"
export BUILD_ROOT=${BUILD_ROOT:-$repo/build/native}
export CARGO_TARGET_DIR="$BUILD_ROOT/rust"
logs="$BUILD_ROOT/logs"
mkdir -p -- "$logs"
export PATH="$HOME/.cargo/bin:$PATH"
repeats=${SIL6250_TEST_REPEATS:-10}
[[ $repeats =~ ^[1-9][0-9]{0,2}$ ]] || { echo 'SIL6250_TEST_REPEATS must be 1..999.' >&2; exit 1; }

logged() { local name=$1; shift; "$@" 2>&1 | tee "$logs/$name.log"; }

logged rust-debug cargo test --workspace --all-targets --locked
logged rust-release cargo test --workspace --all-targets --locked --release
logged clippy cargo clippy -p sil6250-ffi --all-targets --no-deps -- -D warnings
logged installer python3 tests/test_install.py
logged kernel-installer python3 tests/test_kernel_install.py
logged runtime-guards python3 tests/test_runtime.py
logged package-guards python3 tests/test_package.py
logged verification-report python3 tests/test_verification_report.py
logged native ./install.sh build
logged runtime python3 tools/check_runtime.py "$BUILD_ROOT/prefix/lib"

first_cpu=$(python3 -c 'import os; print(min(os.sched_getaffinity(0)))')
for ((attempt=1; attempt<=repeats; attempt++)); do
    logged "single-cpu-$attempt" taskset -c "$first_cpu" "$BUILD_ROOT/meson/tests/test-sil6250"
done

if [[ -z ${FPRINTD:-} ]]; then
    for candidate in /usr/libexec/fprintd /usr/lib/fprintd/fprintd; do
        if [[ -x $candidate ]]; then
            export FPRINTD=$candidate
            break
        fi
    done
fi
logged permissions tests/run_permissions.sh

source /etc/os-release
case "$ID" in
    debian|ubuntu)
        logged package python3 packaging/debian/build.py --build-root "$BUILD_ROOT" \
            --output "$BUILD_ROOT/packages" --maintainer 'CI <ci@example.invalid>'
        ;;
    fedora) echo 'Debian packaging does not apply to Fedora.' | tee "$logs/package.log" ;;
    *) echo "Unsupported CI distribution: $ID" >&2; exit 1 ;;
esac

logged sanitizer-setup meson configure "$BUILD_ROOT/meson" -Db_sanitize=address,undefined -Db_lundef=false
logged sanitizer-build meson compile -C "$BUILD_ROOT/meson"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
    logged sanitizer-tests meson test -C "$BUILD_ROOT/meson" --suite unit-tests --print-errorlogs
