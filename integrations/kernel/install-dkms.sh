#!/usr/bin/env bash
set -euo pipefail
export PATH="$PATH:/usr/sbin:/sbin"
export LC_ALL=C

installation_is_current() {
    [[ "$1" != *$'\n'* ]] &&
        grep -qE ': installed( \((Original modules exist|original_module exists)\))?$' <<< "$1"
}

[[ $(id -u) == 0 && $# == 3 ]] || { echo 'Run via install.sh kernel.' >&2; exit 1; }
source_dir=$(realpath -- "$1")
base_version=$2
kernelver=$3
[[ "$base_version" =~ ^[0-9][A-Za-z0-9._+-]*$ && "$kernelver" =~ ^[A-Za-z0-9._+-]+$ ]] || exit 1
sources=(Makefile dkms.conf sil6250.c sil6250_irq.h sil6250_uapi.h)
exec 9>/run/lock/sil6250-dkms.lock
flock -x 9
temporary=$(mktemp -d /usr/src/.sil6250.XXXXXXXX)
trap 'rm -rf -- "$temporary"' EXIT
for name in "${sources[@]}"; do
    install -m 0644 -- "$source_dir/$name" "$temporary/"
done
chmod 0755 "$temporary"
digest=$(cd -- "$temporary" && sha256sum "${sources[@]}" | sha256sum)
digest=${digest:0:16}
sequence=0
latest_digest=
prefix="/usr/src/sil6250-$base_version+local."
for directory in "$prefix"*; do
    [[ -d "$directory" ]] || continue
    suffix=${directory#"$prefix"}
    number=${suffix%%.src.*}
    hash=${suffix#*.src.}
    if [[ "$number" =~ ^[1-9][0-9]{0,11}$ && "$hash" =~ ^[a-f0-9]{16}$ ]] &&
       (( number > sequence )); then
        sequence=$number
        latest_digest=$hash
    fi
done
if [[ "$latest_digest" != "$digest" ]]; then
    sequence=$((sequence + 1))
fi
version="$base_version+local.$sequence.src.$digest"
destination="/usr/src/sil6250-$version"
status=$(dkms status -m sil6250 -v "$version" -k "$kernelver")
sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$version\"/" "$temporary/dkms.conf"
if [[ -e "$destination" || -L "$destination" ]]; then
    [[ -d "$destination" && ! -L "$destination" ]] || exit 1
    for name in "${sources[@]}"; do
        cmp -s -- "$temporary/$name" "$destination/$name" || {
            echo "Existing DKMS source differs: $destination/$name" >&2
            exit 1
        }
    done
else
    mv -T -- "$temporary" "$destination"
fi
new_registration=false
if [[ -z "$status" ]]; then
    registered=$(dkms status -m sil6250 -v "$version")
    if [[ -z "$registered" ]]; then
        dkms add -m sil6250 -v "$version"
        new_registration=true
    fi
fi
if installation_is_current "$status"; then
    echo "SIL6250 $version is already installed for $kernelver."
    exit 0
fi
previous=$(dkms status -m sil6250 -k "$kernelver" |
    sed -n 's|^sil6250/\([^,]*\),.*: installed.*$|\1|p')
build_options=()
if [[ "$status" == *'(Built modules are missing in the kernel modules folder)'* ||
      "$status" == *'(WARNING! Missing some built modules!)'* ]]; then
    build_options+=(--force)
fi
if ! dkms build -m sil6250 -v "$version" -k "$kernelver" "${build_options[@]}"; then
    if ! "$new_registration" && grep -qE ': added$' <<< "$status"; then
        temporary=$(mktemp -d /usr/src/.sil6250.XXXXXXXX)
        for name in "${sources[@]}"; do
            install -m 0644 -- "$destination/$name" "$temporary/"
        done
        chmod 0755 "$temporary"
        sequence=$((sequence + 1))
        version="$base_version+local.$sequence.src.$digest"
        destination="/usr/src/sil6250-$version"
        sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$version\"/" "$temporary/dkms.conf"
        mv -T -- "$temporary" "$destination"
        dkms add -m sil6250 -v "$version"
        new_registration=true
        if ! dkms build -m sil6250 -v "$version" -k "$kernelver"; then
            dkms remove -m sil6250 -v "$version" --all || echo 'Failed to remove incomplete DKMS registration.' >&2
            exit 1
        fi
    else
        if "$new_registration"; then
            dkms remove -m sil6250 -v "$version" --all || echo 'Failed to remove incomplete DKMS registration.' >&2
        fi
        exit 1
    fi
fi
if dkms install -m sil6250 -v "$version" -k "$kernelver" --force &&
   status=$(dkms status -m sil6250 -v "$version" -k "$kernelver") &&
   installation_is_current "$status"; then
    exit 0
else
    echo 'DKMS installation failed or installed files could not be verified.' >&2
    if "$new_registration"; then
        dkms remove -m sil6250 -v "$version" --all || echo 'Failed to remove incomplete DKMS registration.' >&2
    fi
    if [[ -n "$previous" && "$previous" != *$'\n'* ]]; then
        echo "Restoring previously installed DKMS version $previous." >&2
        dkms install -m sil6250 -v "$previous" -k "$kernelver" --force || {
            echo "Automatic restore failed; retained sources for $previous." >&2
        }
    fi
    exit 1
fi
