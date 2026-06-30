# AUR packaging

Arch Linux packages, an alternative to `install.sh`. Two packages, mirroring
the install stages:

| Package | What it ships | Build |
|---------|---------------|-------|
| [`sil6250-dkms`](sil6250-dkms/) | kernel module + udev rule | DKMS (rebuilt on every kernel upgrade) |
| [`sil6250d`](sil6250d/) | open-fprintd backend daemon, systemd unit, D-Bus policy | cargo |

These are **not on the AUR yet** — build them from a checkout of this repo.
`sil6250d` depends on `sil6250-dkms` (and on
[`open-fprintd`](https://aur.archlinux.org/packages/open-fprintd)), so build the
DKMS package first:

```bash
git clone https://github.com/AlexDaichendt/sil6250-linux.git
cd sil6250-linux

(cd packaging/aur/sil6250-dkms && makepkg -si)
(cd packaging/aur/sil6250d    && makepkg -si)

# sil6250d's install scriptlet loads the module and enables+starts the service.
fprintd-enroll          # or GNOME/KDE Settings
```

> The `sil6250d` package's `.install` scriptlet runs `modprobe sil6250`,
> reloads D-Bus, and `systemctl enable --now sil6250d` on install. Auto-enabling
> a service deviates from Arch packaging guidelines — drop those lines from
> `sil6250d.install` if publishing to the AUR.

`makepkg -si` installs via `pacman`, so removal is `pacman -R sil6250d
sil6250-dkms`. These are VCS (`-git`) packages: they build from the latest
`main`, and the `pkgver()` functions derive a version like `0.1.0.r42.gdeadbee`
from the commit count and hash.

Once published to the AUR this becomes `paru -S sil6250d` (which pulls the whole
stack automatically).

## Publishing to the AUR

Each package is its own AUR git repo. `.SRCINFO` is committed alongside the
`PKGBUILD` and must be regenerated after any PKGBUILD change:

```bash
makepkg --printsrcinfo > .SRCINFO
```

Once a tagged release exists, consider switching the `source=` to a tarball of
the tag and dropping the `-git` suffix for reproducible, pinned packages.
