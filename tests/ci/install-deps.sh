#!/usr/bin/env bash
set -euo pipefail

[[ $(id -u) == 0 ]] || { echo 'Install test dependencies inside the CI container as root.' >&2; exit 1; }
mode=${1:-native}
case "$mode" in native|kernel|all) ;; *) echo 'Usage: install-deps.sh [native|kernel|all]' >&2; exit 1 ;; esac
source /etc/os-release

case "$ID" in
    debian|ubuntu)
        export DEBIAN_FRONTEND=noninteractive
        packages=(ca-certificates curl git python3 build-essential pkg-config binutils util-linux)
        if [[ $mode != kernel ]]; then
            packages+=(libssl-dev libglib2.0-dev libgusb-dev libgudev-1.0-dev
                libpixman-1-dev libcairo2-dev meson ninja-build dbus python3-dbus
                python3-gi fprintd dpkg-dev clang libclang-rt-dev)
        fi
        if [[ $mode != native ]]; then
            packages+=(libelf-dev kmod)
            if [[ $ID == debian ]]; then
                packages+=(linux-headers-amd64)
            else
                packages+=(linux-headers-generic)
            fi
        fi
        apt-get update
        apt-get install -y --no-install-recommends "${packages[@]}"
        ;;
    fedora)
        packages=(ca-certificates curl-minimal git-core python3 gcc gcc-c++ make
            pkgconf-pkg-config binutils util-linux shadow-utils diffutils findutils)
        if [[ $mode != kernel ]]; then
            packages+=(openssl-devel glib2-devel libgusb-devel libgudev-devel
                pixman-devel cairo-devel meson ninja-build dbus-daemon
                python3-dbus python3-gobject-base fprintd clang compiler-rt libasan libubsan)
        fi
        if [[ $mode != native ]]; then
            packages+=(kernel-devel elfutils-libelf-devel perl kmod)
        fi
        dnf install -y --setopt=install_weak_deps=False "${packages[@]}"
        ;;
    *) echo "Unsupported CI distribution: $ID" >&2; exit 1 ;;
esac
