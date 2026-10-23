#!/usr/bin/env bash
set -euo pipefail
export PATH="$PATH:/usr/sbin:/sbin"

[[ $(id -u) != 0 && $# == 2 && "$1" != "$2" ]] || {
    echo 'Usage (regular user in a fresh VM): dkms-lifecycle.sh kernel1 kernel2' >&2
    exit 1
}
systemd-detect-virt --vm >/dev/null
[[ ! -e /dev/sil6250 ]] || { echo 'Physical sensor detected; refusing test.' >&2; exit 1; }
[[ -z $(sudo dkms status -m sil6250) ]] || { echo 'Use a fresh VM without SIL6250 registrations.' >&2; exit 1; }
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/integrations"
cp -- "$repo/install.sh" "$work/"
cp -r -- "$repo/kernel" "$work/"
cp -r -- "$repo/integrations/kernel" "$work/integrations/"

installed_version() {
    sudo dkms status -m sil6250 -k "$1" |
        sed -n 's|^sil6250/\([^,]*\),.*: installed.*$|\1|p'
}

KERNEL_VERSION=$1 bash "$work/install.sh" kernel
original=$(installed_version "$1")
[[ -n "$original" ]]
KERNEL_VERSION=$1 bash "$work/install.sh" kernel
[[ $(installed_version "$1") == "$original" ]]
module_file=$(modinfo -k "$1" -n sil6250)
original_hash=$(sha256sum "$module_file" | cut -d ' ' -f 1)
printf '%s' 'damaged-installed-module' | sudo tee "$module_file" >/dev/null
sudo dkms status -m sil6250 -k "$1" | grep -q 'Differences between built and installed modules'
KERNEL_VERSION=$1 bash "$work/install.sh" kernel
[[ $(installed_version "$1") == "$original" ]]
[[ $(sha256sum "$module_file" | cut -d ' ' -f 1) == "$original_hash" ]]
sudo rm -- "$module_file"
KERNEL_VERSION=$1 bash "$work/install.sh" kernel
[[ $(sha256sum "$module_file" | cut -d ' ' -f 1) == "$original_hash" ]]
cached_module=$(sudo find "/var/lib/dkms/sil6250/$original/$1" -path '*/module/sil6250.ko*' -type f -print)
[[ -n "$cached_module" && "$cached_module" != *$'\n'* ]]
sudo rm -- "$cached_module"
KERNEL_VERSION=$1 bash "$work/install.sh" kernel
sudo dkms status -m sil6250 -v "$original" -k "$1" | grep -qE ': installed( \(Original modules exist\))?$'
[[ -f "$module_file" && -f "$cached_module" ]]
KERNEL_VERSION=$2 bash "$work/install.sh" kernel
[[ $(installed_version "$2") == "$original" ]]
sed -i 's/SIL6250_WAIT_DEFAULT_MS\t500/SIL6250_WAIT_DEFAULT_MS\t501/' "$work/kernel/sil6250.c"
KERNEL_VERSION=$1 bash "$work/install.sh" kernel
updated=$(installed_version "$1")
[[ -n "$updated" && "$updated" != "$original" ]]
[[ -d "/usr/src/sil6250-$original" ]]
sudo dkms autoinstall -k "$2"
[[ $(installed_version "$2") == "$updated" ]]

mkdir "$work/commands"
cat > "$work/commands/dkms" <<'DKMS'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$DKMS_TEST_LOG"
if [[ "$1" == install && " $* " != *" -v $PREVIOUS_VERSION "* ]]; then
    if [[ "$FAILURE_MODE" == after-install ]]; then
        "$REAL_DKMS" "$@"
        printf '%s\n' 'replacement-installed-before-failure' >> "$DKMS_TEST_LOG"
    else
        printf '%s' 'incomplete-module-copy' > "$MODULE_FILE"
        printf '%s\n' 'module-copy-corrupted-before-failure' >> "$DKMS_TEST_LOG"
    fi
    exit 42
fi
exec "$REAL_DKMS" "$@"
DKMS
chmod 0755 "$work/commands/dkms"
base_version=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$work/kernel/dkms.conf")
module_file=$(modinfo -k "$1" -n sil6250)
original_hash=$(sha256sum "$module_file" | cut -d ' ' -f 1)
for failure_mode in after-install partial-copy; do
    if [[ "$failure_mode" == after-install ]]; then
        sed -i 's/SIL6250_WAIT_DEFAULT_MS\t501/SIL6250_WAIT_DEFAULT_MS\t502/' "$work/kernel/sil6250.c"
    else
        sed -i 's/SIL6250_WAIT_DEFAULT_MS\t502/SIL6250_WAIT_DEFAULT_MS\t503/' "$work/kernel/sil6250.c"
    fi
    if sudo env PATH="$work/commands:$PATH" REAL_DKMS="$(command -v dkms)" \
        DKMS_TEST_LOG="$work/$failure_mode.log" PREVIOUS_VERSION="$updated" \
        FAILURE_MODE="$failure_mode" MODULE_FILE="$module_file" \
        bash "$work/integrations/kernel/install-dkms.sh" "$work/kernel" "$base_version" "$1"; then
        echo 'Fault injection unexpectedly succeeded.' >&2
        exit 1
    fi
    [[ $(installed_version "$1") == "$updated" ]]
    [[ $(sha256sum "$module_file" | cut -d ' ' -f 1) == "$original_hash" ]]
    grep -q "install -m sil6250 -v $updated " "$work/$failure_mode.log"
done

cat > "$work/commands/dkms" <<'DKMS'
#!/usr/bin/env bash
set -euo pipefail
if [[ "$1" == install ]]; then
    if [[ "$INTERRUPTION_MODE" == partial-copy ]]; then
        "$REAL_DKMS" "$@"
        printf '%s' 'interrupted-module-copy' > "$MODULE_FILE"
    fi
    printf '%s\n' "$INTERRUPTION_MODE" > "$INTERRUPTION_MARKER"
    kill -KILL "$PPID"
    exit 0
fi
exec "$REAL_DKMS" "$@"
DKMS
for interruption_mode in before-install partial-copy; do
    if [[ "$interruption_mode" == before-install ]]; then
        sed -i 's/SIL6250_WAIT_DEFAULT_MS\t503/SIL6250_WAIT_DEFAULT_MS\t504/' "$work/kernel/sil6250.c"
    else
        sed -i 's/SIL6250_WAIT_DEFAULT_MS\t504/SIL6250_WAIT_DEFAULT_MS\t505/' "$work/kernel/sil6250.c"
    fi
    interrupted_status=0
    sudo env PATH="$work/commands:$PATH" REAL_DKMS="$(command -v dkms)" \
        INTERRUPTION_MODE="$interruption_mode" MODULE_FILE="$module_file" \
        INTERRUPTION_MARKER="$work/$interruption_mode.marker" \
        bash "$work/integrations/kernel/install-dkms.sh" "$work/kernel" "$base_version" "$1" || interrupted_status=$?
    [[ "$interrupted_status" == 137 ]]
    grep -qx "$interruption_mode" "$work/$interruption_mode.marker"
    KERNEL_VERSION=$1 bash "$work/install.sh" kernel
    updated=$(installed_version "$1")
    [[ -n "$updated" && "$updated" != "$original" ]]
    status=$(sudo dkms status -m sil6250 -v "$updated" -k "$1")
    grep -qE ': installed( \(Original modules exist\))?$' <<< "$status"
    [[ -d "/usr/src/sil6250-$original" ]]
done
sudo dkms autoinstall -k "$2"
[[ $(installed_version "$2") == "$updated" ]]
sudo dkms status -m sil6250
echo 'DKMS repeat, repair, update, autoinstall, rollback and interrupted retry checks passed.'
