# SIL6250 fingerprint driver for Linux

This fork integrates the Silead SIL6250 / Petaic match-on-host sensor with
standard fprintd through libfprint. The Rust engine handles the sensor's
64×80 images, EC mailbox transport, TLS-PSK connection and matching. A C
interface exposes that engine to the libfprint driver.

The standalone sil6250d/open-fprintd backend is no longer built or shipped.
The distribution's fprintd provides authorization, device claiming and
fingerprint storage. Desktop applications and PAM use its normal D-Bus API.
This requires our libfprint patches; an unmodified distribution libfprint
does not acquire SIL6250 support simply by installing fprintd.

> This repo was created with assistance from various LLMs. If you prefer your
> code to be written only by humans, please move on.

## Build and test

Use a current stable Rust toolchain, preferably through rustup. Build as your
regular user. Requirements include Git, Make, C/C++ compilers, Meson, Ninja,
pkg-config, OpenSSL development files and libfprint's GLib/GIO, libgusb,
GUdev, pixman and cairo build dependencies.

```sh
./install.sh
```

The default action builds and tests into `build/native/prefix`. It does not
request root access, copy system files, change services, modify PAM or enroll
fingerprints. It checks out the pinned revision in
`integrations/libfprint/revision`, applies the driver patches and runs C/C++
ABI and Meson tests. Missing device-replay dependencies can result in skipped
tests; report those separately from passes.

The driver selection is `default,sil6250`, preserving libfprint's other
default drivers. For the complete virtual-driver test set, use
`LIBFPRINT_DRIVERS=all,sil6250`. Explicit comma-separated driver lists also
work. `BUILD_ROOT`, `CARGO` and `MESON` are configurable. When driver patches
change, use a fresh `BUILD_ROOT` rather than discarding local source edits.

Rust tests run independently:

```sh
cargo test --workspace --all-targets --locked
python3 -m unittest discover -s tests
```

The bridge requires fresh random PSK provisioning by default. A failed key
setup or handshake fails the operation without trying a public fallback key.
`SIL6250_PSK_MODE=auto|provision|replay|legacy` can be set in fprintd's
environment; invalid values fail engine initialization. `auto`, `replay` and
`legacy` are explicit compatibility modes that permit public keys. New staged
installations and Debian packages select `provision`. Existing local overrides
may still select a compatibility mode and require a separate migration test.
Provisioning transfers an obfuscated key over the mailbox; it does not provide
a hardware trust anchor against an attacker who can observe or control that
mailbox. Keep direct device access restricted to the privileged service.
Physical spoof resistance and population false-acceptance rates have not been
established by the single-device functional tests.
See `sil6250-ffi/ABI`
for ownership, error conventions and timeout limits. Verification allows up
to three separate presses at the existing matching threshold. Descriptor
pairs are evaluated once for both nearest-neighbor directions. The driver
stops matching further reference samples after the unchanged acceptance
threshold is met; malformed galleries are rejected before that check.

## Prepare a system installation

The userspace deployment requires the distribution's standard fprintd,
PolicyKit policy and PAM integration. Keep its vendor service and D-Bus
activation files. No replacement authentication daemon or policy is supplied
by this fork.

Build a staging tree without changing the running system:

```sh
./install.sh build stage
```

`stage` places the two runtime libraries and a standard `fprintd.service`
override under `build/native/stage`. `PREFIX` defaults to `/usr/local`;
`DESTDIR` changes the staging directory. This step never installs to `/`.
It does not remove existing fingerprint services or records. Staging refuses
an existing destination, including symbolic links. Files are assembled in a
private temporary directory and renamed into place only after all copies
succeed. For a subsequent staging run, choose a fresh `DESTDIR`.

Before activating the libraries, finish an isolated hardware test of this
revision, retain a working alternative login method, and back up the existing
service configuration and enrollment storage. Only one implementation may
own `net.reactivated.Fprint`. Remove or disable the legacy stack deliberately
through its package manager; do not run both services on the sensor.

For the default PREFIX, reviewed manual deployment consists of installing
`libfprint-2.so.2.0.0` and `libsil6250_ffi.so.0` into
`/usr/local/lib/sil6250`, adding the `libfprint-2.so.2` symlink and installing
`60-sil6250.conf` into `/etc/systemd/system/fprintd.service.d/`. These files
and every parent of the library directory must be root-owned and not writable
by ordinary users. Preserve existing overrides for rollback. Reload systemd
and restart standard fprintd only when ready to test authentication.

The override selects these libraries for fprintd only and allows access to
`/dev/sil6250`. It preserves the vendor's authorization and service settings.
Other applications continue to use the distribution's installed libfprint.
The override also applies an owner-only creation mask and restricts core-dump
memory. Selected owned temporary key and capture buffers are cleared when released;
this does not guarantee removal of compiler-generated copies, third-party
allocations or data already captured in swap or diagnostics. Fingerprint records
remain under `/var/lib/fprint`; the old standalone
backend's records are not automatically converted or deleted. Enroll with
`fprintd-enroll` after validating the new service.

The kernel module can be installed separately with `./install.sh kernel`.
This is an explicit system-changing step and requests sudo. DKMS and the
headers for installed kernels are needed for automatic rebuilding. See
`kernel/README.md` for compiler and Secure Boot requirements. The kernel udev
rule keeps raw device access restricted to root.

`./install.sh check` reports the standard service's executable and state
without starting it. An old service name may still wrap a native fprintd
installation; inspect ExecStart before deciding how to migrate it.

## Architecture

```text
GNOME / KDE / PAM
       | D-Bus
standard fprintd
       | libfprint API
patched libfprint SIL6250 driver
       | C ABI
sil6250-ffi -> Rust sil6250 engine
       | mmap / ioctl
sil6250.ko -> EC mailbox -> sensor
```

`REVERSE_ENGINEERING_DETAILS.md` preserves the original protocol research.
Its standalone-daemon architecture describes the historical implementation.

`tools/benchmark_matching.py` compares a trusted baseline shared library
against a new one using synthetic images only. It checks full scores and
acceptance decisions before reporting median timings. These timings measure
matching computation, not sensor latency or a biometric false-accept rate.

## Validation limits

Random provisioning removes the default public-key fallback, but the key blob
is only obfuscated on the mailbox. An attacker who can observe or control that
link can recover the key. This sensor protocol has no verified hardware trust
anchor; provisioning does not establish resistance to an active physical
attacker. PSK frames and plaintext keys are excluded from diagnostic reports.

This remains an experimental integration. Earlier isolated verification,
GNOME unlocking and reboot tests used the engine before upstream's recent
PSK provisioning update. They do not establish hardware reliability of the
updated branch. Suspend/resume and additional devices need separate testing;
small samples do not establish a general false-accept rate.

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

## Testing and packaging

See [TESTING.md](TESTING.md) for isolated permission tests, fuzzing, hardware
validation and local Debian package builds. These checks do not require changing
PAM or replacing a working login service.

## License

Original license and copyright notices are retained. The root LICENSE has
LGPL-2.1 text, while the Rust engine carries GPL-2.0 notices and depends on
Apache-2.0 code. Clarify the applicable permissions before distributing a
combined binary. The C interface's LGPL-2.1-or-later license does not
relicense the engine or grant a linking exception.
