# libfprint driver overlay

libfprint compiles its drivers *into* `libfprint.so` and has no out-of-tree
driver ABI, so the SIL6250 driver cannot be a standalone plugin. It ships as a
small **overlay** on a pinned upstream libfprint checkout:

- `sil6250.c` — the driver: a thin `FpDevice` subclass that links `libsil6250`
  for all capture and matching. Copy it to
  `libfprint/libfprint/drivers/sil6250/sil6250.c`.
- `libfprint.patch` — adds `sil6250` to the driver list and wires a `sil6250`
  build helper that resolves `libsil6250` via **pkg-config** (the same mechanism
  libfprint already uses for `uru4000`→openssl). Also carries a one-line
  `tests/meson.build` fix for newer meson's dict iteration.
- `fprintd-sil6250.conf` — systemd drop-in exporting `FP_SIL6250=/dev/sil6250`
  so fprintd auto-binds the device (see the top-level README, "Discovery").

`../install.sh libfprint` applies all of this and rebuilds libfprint. To do it
by hand, see the top-level README. Build `../lib` (libsil6250) **first** — the
patch's pkg-config lookup depends on it being installed.

## Why a separate library, not in-tree sources?

It keeps the forked libfprint surface to a single adapter file, and lets the
protocol + matcher be developed and tested independently of libfprint. Note that
if this driver is ever submitted upstream, libfprint convention (cf. the
in-tree, vendored NBIS) would favour bringing those sources in-tree; the overlay
patch is the natural seam for that change.
