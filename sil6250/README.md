# sil6250 — Rust library crate

The userspace core of the SIL6250 stack — everything above the kernel broker.
Builds as a regular Rust library (`lib`) and is used directly by `sil6250d`.

| Module          | Responsibility                                                   |
|-----------------|------------------------------------------------------------------|
| `proto.rs`      | Mailbox frame build / parse / checksum                           |
| `transport.rs`  | `/dev/sil6250` round-trip (shm write → strobe → IRQ → read), over `sil6250_uapi.h` UAPI |
| `engine.rs`     | TLS-PSK secure channel + the `0x11→0x37→0x38` capture loop; blocking API |
| `matcher.rs`    | Frame destripe + enroll-diversity NCC gate                       |
| `sift.rs`       | Clean-room SIFT-128 + geometric-consistency matcher + quality gate |

`transport.rs` opens `/dev/sil6250` and speaks the kernel UAPI directly
(ioctl + mmap); `sil6250_uapi.h` is the single source of truth for that ABI.

## Build

```sh
cargo build              # from the workspace root
cargo test               # offline matcher unit tests
```

This crate is not published to crates.io; it is a workspace member of the
top-level `Cargo.toml` consumed by `sil6250d`.
