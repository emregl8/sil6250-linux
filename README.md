# SIL6250 fingerprint driver for Linux

A complete Linux stack for the **Silead SIL6250 / Petaic** match-on-host
fingerprint sensor (as found in the Huawei MateBook X Pro 2024), so it works
with the normal desktop fingerprint experience: enrol in GNOME/KDE Settings, log
in and `sudo` with your finger via `fprintd` / PAM.

The sensor is unusual: it is a 64×80 touch reader reached over an
**EC-arbitrated shared-memory mailbox**, not USB or SPI, behind a TLS-PSK secure
channel. There is no public driver. This repo is a clean, self-contained
reimplementation built from reverse engineering.

## Architecture

```
  GNOME/KDE Settings, PAM (sudo/login)
            │  D-Bus
          fprintd
            │
        libfprint ── drivers/sil6250/sil6250.c   ← thin FpDevice adapter
            │                  │ links
            │            ┌─────┴───────────────────────────────┐
            │            │  libsil6250  (this repo, lib/)       │
            │            │  • mailbox framing  (petaic_proto)   │
            │            │  • TLS-PSK + capture (petaic_engine) │
            │            │  • host matcher   (petaic_match/sift)│
            │            └─────┬───────────────────────────────┘
            │                  │ /dev/sil6250  (mmap + ioctl)
            │            sil6250.ko  (this repo, kernel/)   ← resource broker
            │                  │ ACPI platform device "SIL6250"
        hardware ─────────────┘  EC mailbox window + GPIO strobes + IRQ
```

The design keeps each layer as thin as it can be:

- **`kernel/` — `sil6250.ko`**: a minimal ACPI platform driver with *no
  protocol logic*. It ioremaps the mailbox window and exposes it as
  `/dev/sil6250` (`mmap` the window, `ioctl` for the GPIO strobes + IRQ wait).
- **`lib/` — `libsil6250`**: where all the real work lives — framing, the
  TLS-PSK secure channel, the `0x11 → 0x37 → 0x38` capture loop, and the
  host-side matcher (the sensor only streams raw images; the 3.2×4 mm patch is
  too small for the NBIS minutiae pipeline). Independently buildable and
  testable; the matcher is a patent-free clean-room reimplementation.
- **`fprint-driver/` — the libfprint driver**: a small `FpDevice` adapter that
  links `libsil6250`. libfprint has no out-of-tree driver ABI, so it ships as an
  *overlay* (one source file + a patch) applied to a pinned upstream libfprint.

## Layout

```
kernel/          ACPI mailbox broker (sil6250.ko), UAPI header, udev rule, DKMS
lib/             libsil6250 — userspace core (meson library + headers + pkg-config)
tools/           CLI utilities: capture, TLS smoke test, offline matcher ROC
fprint-driver/   libfprint FpDevice driver + overlay patch + fprintd drop-in
libfprint/       upstream libfprint (git submodule; populated by install.sh)
install.sh       orchestrates build + install of all four layers
```

## Quick start

```sh
# build + install everything (libsil6250, kernel module, fprintd glue, libfprint)
./install.sh

# then enrol
fprintd-enroll          # or use GNOME/KDE Settings
```

`install.sh` runs four stages — `lib`, `kernel`, `fprintd`, `libfprint` — and
you can run any subset, e.g. `./install.sh lib kernel`. It uses `sudo` only for
the steps that install system-wide. Override the install prefix with
`PREFIX=/usr` and the libfprint version with `LIBFPRINT_REF=...`.

## Building components by hand

```sh
# libsil6250 + CLI tools
meson setup build && meson compile -C build && sudo meson install -C build

# kernel module (DKMS recommended; plain make also works)
make -C kernel                       # add LLVM= if your kernel is gcc-built
sudo make -C kernel modules_install && sudo depmod -a
sudo install -Dm644 kernel/60-sil6250.rules /etc/udev/rules.d/60-sil6250.rules
sudo udevadm control --reload && sudo modprobe sil6250

# libfprint driver overlay (against the pinned submodule)
git submodule update --init libfprint
install -Dm644 fprint-driver/sil6250.c libfprint/libfprint/drivers/sil6250/sil6250.c
git -C libfprint apply ../fprint-driver/libfprint.patch
meson setup libfprint/build libfprint -Ddrivers=default   # finds libsil6250 via pkg-config
sudo meson install -C libfprint/build
```

## Discovery

The mailbox `/dev/sil6250` is neither `hidraw` nor `spidev`, so libfprint's udev
backend cannot enumerate it. The driver therefore registers as a **virtual-type
device** keyed on the `FP_SIL6250` environment variable, whose value is the
device-node path. `fprint-driver/fprintd-sil6250.conf` is a systemd drop-in that
exports `FP_SIL6250=/dev/sil6250` into the `fprintd` service environment, so the
device is bound automatically whenever fprintd is activated — no manual step at
login. (Full udev auto-discovery would require patching libfprint core to scan a
custom subsystem; deferred to keep the forked surface tiny.)

## Requirements

- A kernel with headers (DKMS or `make` against `/lib/modules/$(uname -r)/build`)
- `meson`, `ninja`, a C compiler
- `mbedtls` (`mbedtls`, `mbedx509`, `mbedcrypto`) — the TLS-PSK secure channel
- `glib`, `gusb`, `gudev` — libfprint's own dependencies
- `fprintd` + a PAM/desktop frontend for actual login use

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
- The [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) and
  [fprintd](https://gitlab.freedesktop.org/libfprint/fprintd) projects, on which
  the desktop integration is built.

## License

LGPL-2.1 (see `LICENSE`).
