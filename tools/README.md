# Tools

CLI utilities, all thin consumers of `libsil6250`. Built by default with the top
level (`meson compile -C build`); disable with `-Dtools=false`. They are not
installed — run them from the build tree (`build/tools/<name>`).

| Tool              | Purpose                                                       |
|-------------------|--------------------------------------------------------------|
| `petaic_record`   | Raw mailbox round-trip smoke test (no TLS) — bring-up        |
| `petaic_tls`      | TLS-PSK handshake smoke test                                 |
| `petaic_capture`  | Standalone capture to PGM (handshake + capture loop)         |
| `petaic_demo`     | Engine-API capture demo                                      |
| `qlive`           | Live enroll/verify against the host matcher                  |
| `petaic_roc`      | Offline NCC matcher ROC over PGM sets                        |
| `petaic_sift_roc` | Offline SIFT matcher ROC over PGM sets                       |

The hardware tools need `/dev/sil6250` (load `sil6250.ko` and install the udev
rule first). The `*_roc` tools are offline and operate on PGM image sets passed
on the command line.
