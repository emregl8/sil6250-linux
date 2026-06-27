# SIL6250 fingerprint driver for Linux

A complete Linux stack for the **Silead SIL6250 / Petaic** match-on-host
fingerprint sensor (as found in the Huawei MateBook X Pro 2024), so it works
with the normal desktop fingerprint experience: enrol in GNOME/KDE Settings, log
in and `sudo` with your finger via PAM.

The sensor is unusual: it is a 64×80 touch reader reached over an
**EC-arbitrated shared-memory mailbox**, not USB or SPI, behind a TLS-PSK secure
channel. There is no public driver. This repo is a clean, self-contained
reimplementation built from reverse engineering.

## Architecture

```
  GNOME/KDE Settings, PAM (sudo/login)
            │  D-Bus
          open-fprintd
            │  io.github.uunicorn.Fprint.Device
          sil6250d  (this repo, open-fprintd-driver/)   ← standalone Rust daemon
            │ links
      ┌─────┴───────────────────────────────┐
      │  libsil6250  (this repo, lib/)       │
      │  • mailbox framing  (petaic_proto)   │
      │  • TLS-PSK + capture (petaic_engine) │
      │  • host matcher   (petaic_match/sift)│
      └─────┬───────────────────────────────┘
            │ /dev/sil6250  (mmap + ioctl)
      sil6250.ko  (this repo, kernel/)   ← resource broker
            │ ACPI platform device "SIL6250"
    hardware   EC mailbox window + GPIO strobes + IRQ
```

Each layer is as thin as possible:

- **`kernel/` — `sil6250.ko`**: a minimal ACPI platform driver with *no
  protocol logic*. It ioremaps the mailbox window and exposes it as
  `/dev/sil6250` (`mmap` the window, `ioctl` for the GPIO strobes + IRQ wait).
- **`lib/` — `libsil6250`**: where all the real work lives — framing, the
  TLS-PSK secure channel, the `0x11 → 0x37 → 0x38` capture loop, and the
  host-side matcher (the sensor only streams raw images; the 3.2×4 mm patch is
  too small for the NBIS minutiae pipeline). Independently buildable and
  testable; the matcher is a patent-free clean-room reimplementation.
- **`open-fprintd-driver/` — `sil6250d`**: a Rust daemon that implements the
  `io.github.uunicorn.Fprint.Device` D-Bus interface and registers with
  [open-fprintd](https://github.com/uunicorn/open-fprintd). Enrol/verify/identify
  run as async D-Bus calls; the blocking libsil6250 work runs on a thread pool.
  Enrolled prints are stored in `/var/lib/open-fprintd/sil6250/`.

## Layout

```
kernel/                ACPI mailbox broker (sil6250.ko), UAPI header, udev rule, DKMS
lib/                   libsil6250 — userspace core (meson library + headers + pkg-config)
tools/                 CLI utilities: capture, TLS smoke test, offline matcher ROC
open-fprintd-driver/   sil6250d — Rust open-fprintd backend daemon
install.sh             orchestrates build + install of all three layers
```

## Quick start

Install [open-fprintd](https://github.com/uunicorn/open-fprintd) first (your
distro may package it), then:

```sh
# build + install everything (libsil6250, kernel module, sil6250d daemon)
./install.sh

# enrol
fprintd-enroll          # or use GNOME/KDE Settings
```

`install.sh` runs three stages — `lib`, `kernel`, `daemon` — and you can run
any subset, e.g. `./install.sh lib kernel`. It uses `sudo` only for steps that
install system-wide. Override the install prefix with `PREFIX=/usr`.

## Building components by hand

```sh
# libsil6250 + CLI tools
meson setup build && meson compile -C build && sudo meson install -C build

# kernel module (DKMS recommended; plain make also works)
make -C kernel                       # add LLVM= if your kernel is gcc-built
sudo make -C kernel modules_install && sudo depmod -a
sudo install -Dm644 kernel/60-sil6250.rules /etc/udev/rules.d/60-sil6250.rules
sudo udevadm control --reload && sudo modprobe sil6250

# sil6250d daemon
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
  cargo build --release --manifest-path open-fprintd-driver/Cargo.toml
sudo install -Dm755 open-fprintd-driver/target/release/sil6250d /usr/local/bin/sil6250d
sudo install -Dm644 open-fprintd-driver/io.github.uunicorn.Fprint.conf \
  /etc/dbus-1/system.d/io.github.uunicorn.Fprint.conf
sudo install -Dm644 open-fprintd-driver/sil6250d.service \
  /etc/systemd/system/sil6250d.service
sudo systemctl daemon-reload && sudo systemctl enable --now sil6250d
```

## Requirements

- A kernel with headers (DKMS or `make` against `/lib/modules/$(uname -r)/build`)
- `meson`, `ninja`, a C compiler
- `mbedtls` (`mbedtls`, `mbedx509`, `mbedcrypto`) — the TLS-PSK secure channel
- `cargo` / Rust toolchain — for `sil6250d`
- [open-fprintd](https://github.com/uunicorn/open-fprintd) — the fprintd replacement
- A PAM/desktop frontend (`fprintd-enroll`, GNOME/KDE Settings) for actual login use

## How it was built

The sensor is undocumented and has no public driver; this stack was produced by
reverse engineering. The full story — hardware identification, the EC mailbox
transport, the TLS-PSK channel, and the clean-room reimplementation of the
match-on-host algorithm — is in **`REVERSE_ENGINEERING_DETAILS.md`**.

## Credits

- [**Void755/gxfp_linux_driver**](https://github.com/Void755/gxfp_linux_driver)
  — the open-source GXFP5130 driver for the Goodix sibling sensor on the same
  `\_SB.SPBA` ACPI node and `0xFE800000` mailbox window. Its working, ACK-getting
  kernel code was the verified reference for the EC mailbox transport (the TX/RX
  window split, the GPIO handshake strobes, and the EC arbitration model). See
  `REVERSE_ENGINEERING_DETAILS.md` §3.2.
- [**uunicorn/open-fprintd**](https://github.com/uunicorn/open-fprintd) — the
  fprintd-compatible daemon that accepts out-of-tree device backends over D-Bus,
  making a standalone driver possible without patching libfprint.

## License

LGPL-2.1 (see `LICENSE`).
