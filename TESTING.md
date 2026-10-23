# Tests

Run the Rust and installer tests without administrator privileges:

```sh
cargo test --workspace --locked
cargo test --workspace --locked --release
cargo clippy -p sil6250-ffi --all-targets --no-deps -- -D warnings
python3 tests/test_install.py
./install.sh build
```

The native build runs libfprint's available tests, including SIL6250's mocked
capture, cancellation, enrollment and identification tests. A skipped device
replay test is not a hardware test pass.

## IRQ startup regression

The IRQ runtime CI job boots a packaged kernel under QEMU using a small
BusyBox initramfs. It loads the regression module only inside that guest,
then requires a copy without `IRQF_NO_AUTOEN` to fail with `EPROTO`. To run
the same check locally as a regular user, install QEMU, static BusyBox,
fakeroot, cpio and matching kernel headers:

```sh
bash tests/ci/irq-vm.sh /path/to/vmlinuz /path/to/configured/kernel/headers
```

This uses CPU emulation without requiring KVM, network access, a guest disk,
or privileged host module loading. Logs are saved under `build/kernel-runtime`.

The kernel build helper also compiles the synthetic IRQ regression module.
To execute it, use a disposable VM with headers matching its running kernel
and no SIL6250 device attached:

```sh
make -C tests/kernel
sudo bash tests/kernel/run.sh "$PWD/tests/kernel/irq_startup_test.ko"
```

The test reproduces the original double-disable startup race, then exercises
the driver's masked IRQ registration and rearming through the real kernel IRQ
core 64 times. It checks that early notifications stay pending until the first
enable, that the controller starts once, and that subsequent notifications
remain deliverable. The wrapper refuses execution outside a VM or when
`/dev/sil6250` exists. This synthetic controller does not test physical GPIO,
ACPI routing, or sensor behavior.

## Temporary memory

Rust uses `zeroize` for owned PSK plans, callback keys, transmit buffers,
decrypted capture buffers and parsed features. The native worker clears its
temporary image and feature arrays on normal, cancellation and error exits.
Enrollment arrays reserve their maximum size to avoid leaving copies behind
during growth. Staged service overrides set `LimitCORE=0` and
`CoredumpFilter=0x0`; existing deployments require an explicit update.
These measures do not guarantee that compiler temporaries, external-library
allocations, abrupt process termination or previously written swap are cleared.

## Permissions

Install the build dependencies and Python's dbus and GI modules, then run:

```sh
FPRINTD=/path/to/standard/fprintd tests/run_permissions.sh
```

This builds a separate virtual-device library and runs the actual fprintd
executable on a private D-Bus with a mock authorization service and temporary
storage. It tests denied claims, cross-user access, separation of verification
and enrollment permissions, ownership of a device claim, permission revocation,
and cleanup after a client disconnects during enrollment. It never uses the
system bus or a physical reader. The mock policy must not be installed as a
system service.

## Fuzzing

```sh
rustup toolchain install nightly --profile minimal
cargo install cargo-fuzz --locked
python3 tools/seed_fuzz.py
cargo +nightly fuzz run parsers -- -max_total_time=60 -max_len=131072
cargo +nightly fuzz run gallery -- -max_total_time=60 -max_len=131072
cargo +nightly fuzz run images -- -max_total_time=60 -max_len=5120
```

These targets exercise mailbox and TLS identity parsers, feature and gallery
decoding, prepared gallery ownership, and image processing. Seeds contain
synthetic data only. Keep real biometric images and enrollment data out of the
corpus and crash reports. A bounded fuzz run does not prove the absence of bugs.

## Hardware validation

Use an isolated fprintd instance and a protected copy of enrollment records
before updating a working authentication service. Check registered fingers,
unregistered fingers, repeated placement, cancellation, client disconnect and
service reactivation. Test reboot and suspend separately with the user's consent.

Record first-attempt and final results separately: one verification can accept
up to three distinct placements. Measure latency from both request and finger
placement, and report timeouts separately. Matching thresholds and retry counts
must remain fixed when comparing performance.

Wrong-finger tests on one reader are functional checks, not a population false
acceptance estimate or a spoof-resistance certification. A wider evaluation
requires consented independent participants, separate enrollment and probe
samples, and a stated number of genuine and impostor comparisons. Do not tune
and evaluate a threshold using the same samples.

## Local Debian packages

```sh
python3 packaging/debian/build.py --maintainer 'Name <email>'
```

The development package contains private runtime libraries and an override for
standard fprintd; it does not include a replacement fprintd daemon or kernel
module. It conflicts with the legacy daemons. Install the kernel module
separately. Runtime dependencies are calculated from the build host, so build
for each supported distribution release and architecture.

The packaged override is under `/usr/lib/systemd/system`; local overrides in
`/etc` take precedence. Review existing `LD_LIBRARY_PATH` overrides before
installing over a custom deployment. Package removal removes its own override
without leaving a configuration file pointing to removed libraries.

Package scripts only reload systemd unit definitions. They do not restart an
active scan, modify PAM, change enrollment records, or reboot. Idle activation
will use the updated libraries. Keep the previous package for rollback with
the package manager. This builder is for local development; the combined-binary
licensing question in README must be resolved before redistribution.
