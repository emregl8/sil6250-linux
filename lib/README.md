# libsil6250

The userspace core of the SIL6250 stack — everything above the kernel broker.
Builds as a shared library with installed headers and a pkg-config file, so both
the CLI tools (`../tools`) and the libfprint driver (`../fprint-driver`) link it
the same way.

| Module                | Responsibility                                            |
|-----------------------|-----------------------------------------------------------|
| `petaic_proto.{c,h}`  | Mailbox frame build / parse / checksum                    |
| `petaic_transport.*`  | The `/dev/sil6250` round-trip (shm write → strobe → IRQ → read), over `sil6250_uapi.h` |
| `petaic_engine.*`     | TLS-PSK secure channel + the `0x11→0x37→0x38` capture loop; blocking API |
| `petaic_match.*`      | Frame destripe + best-shift NCC (enroll-diversity gate)   |
| `petaic_sift.*`       | Clean-room SIFT-128 + geometric-consistency matcher + quality gate |

Public headers install under `<prefix>/include/sil6250/` and are included flat
(`#include "petaic_engine.h"`); the generated `libsil6250.pc` adds that directory
to consumers' include path.

```sh
meson setup build && meson compile -C build && sudo meson install -C build
```

`transport.c` includes the kernel UAPI header from `../kernel/sil6250_uapi.h`
(added to the build include path); it is the single source of truth for the
userspace↔kernel ABI and is not duplicated here.
