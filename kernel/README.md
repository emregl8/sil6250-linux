# sil6250.ko — ACPI mailbox broker

A minimal Linux platform driver for the SIL6250 fingerprint sensor. It is a pure
**resource broker with no protocol logic**: all framing, crypto, and matching
live in userspace (`../lib`, `libsil6250`).

It binds ACPI HID `SIL6250`, ioremaps the EC mailbox window, and claims the two
GpioIo strobe lines plus the GpioInt. It exposes `/dev/sil6250`:

- `mmap()` the mailbox window (TX at `+0x000`, RX at `+0x200`)
- `ioctl SIL6250_SET_GPIO {line, value}` — line 0 = `write_done`, 1 = `read_done`
- `ioctl SIL6250_WAIT_IRQ {timeout_ms}` — re-arms the masked level line and blocks
- `ioctl SIL6250_GET_WINDOW_SIZE`

The userspace↔kernel ABI is `sil6250_uapi.h` (consumed by `libsil6250`).

## Build

```sh
make                       # builds against the running kernel
sudo make modules_install && sudo depmod -a
```

This kernel Makefile defaults to `LLVM=1` because some distro kernels (e.g.
CachyOS) are clang-built and out-of-tree modules must match the compiler. **If
your kernel was built with gcc, use `make LLVM=`.**

## DKMS (survives kernel upgrades)

```sh
sudo cp -r . /usr/src/sil6250-0.1.0
sudo dkms add sil6250/0.1.0
sudo dkms install sil6250/0.1.0
```

## Permissions

Install `60-sil6250.rules` to `/etc/udev/rules.d/` for non-root access to
`/dev/sil6250` (uaccess ACL for the active session + a `root:users` 0660
fallback for system services like fprintd).
