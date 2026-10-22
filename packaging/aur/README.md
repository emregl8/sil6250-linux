# Arch Linux packaging

`sil6250-dkms/` packages the kernel module and root-only udev rule. It is not
published in the AUR; build it locally with `makepkg -si` in that directory.
Its VCS source follows this fork's `feat/libfprint-backend` branch.

The userspace path uses the distribution's standard `fprintd`, PolicyKit
policy and PAM module, plus the patched libfprint and Rust bridge. Build and
stage those libraries with `./install.sh build stage` from the repository
root. This fork no longer packages the standalone open-fprintd backend.
There is no replacement userspace Arch package yet; the staged libraries
are for a reviewed manual installation, not a pacman-managed replacement.

Keep the existing fingerprint enrollment and a working alternative login
method until the native stack has passed hardware tests. No package script
migrates records or changes PAM configuration.

Regenerate `.SRCINFO` with `makepkg --printsrcinfo` after changing PKGBUILD.
