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
