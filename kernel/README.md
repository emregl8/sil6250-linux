# sil6250.ko — ACPI mailbox broker

A minimal Linux platform driver for the SIL6250 fingerprint sensor. It is a
pure **resource broker with no protocol logic**: all framing, crypto, and
matching live in userspace (`../sil6250`, the Rust library crate).

It binds ACPI HID `SIL6250`, ioremaps the EC mailbox window, and claims the two
GpioIo strobe lines plus the GpioInt. It exposes `/dev/sil6250`:

- `mmap()` the mailbox window (TX at `+0x000`, RX at `+0x200`)
- `ioctl SIL6250_SET_GPIO {line, value}` — line 0 = `write_done`, 1 = `read_done`
- `ioctl SIL6250_WAIT_IRQ {timeout_ms}` — re-arms the masked level line and blocks
- `ioctl SIL6250_GET_WINDOW_SIZE`

The userspace↔kernel ABI is `sil6250_uapi.h` (consumed by the `sil6250` crate).

## Build

```sh
make                       # builds against the running kernel
sudo make modules_install && sudo depmod -a
```

The Makefile detects Clang-built kernels and selects `LLVM=1` automatically.
GCC-built kernels use the default toolchain. Override with `make LLVM=1` or
`make LLVM=` when needed to match the target kernel's compiler.

## DKMS (survives kernel upgrades)

```sh
sudo cp -r . /usr/src/sil6250-0.1.0
sudo dkms add sil6250/0.1.0
sudo dkms install sil6250/0.1.0
```

## Permissions

Install `69-sil6250.rules` to `/etc/udev/rules.d/` to restrict `/dev/sil6250`
to `root`. The root-owned standard `fprintd` service accesses the
raw mailbox through the patched libfprint driver. Desktop applications use
fprintd over D-Bus instead of opening the device directly. Its filename also makes it
override the legacy `60-sil6250.rules` before systemd applies `uaccess` ACLs.
