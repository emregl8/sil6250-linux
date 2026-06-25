# CLI utilities, all thin consumers of libsil6250 (sil6250_dep).
#
#   petaic_record    raw mailbox round-trip smoke test (no TLS)
#   petaic_tls       TLS-PSK handshake smoke test
#   petaic_capture   standalone capture-to-PGM (handshake + 0x11/0x37/0x38 loop)
#   petaic_demo      engine-API capture demo
#   qlive            live enroll/verify against the host matcher
#   petaic_roc       offline NCC matcher ROC over PGM sets
#   petaic_sift_roc  offline SIFT matcher ROC over PGM sets

tools = [
  'petaic_record',
  'petaic_tls',
  'petaic_capture',
  'petaic_demo',
  'qlive',
  'petaic_roc',
  'petaic_sift_roc',
]

foreach t : tools
  executable(
    t,
    t + '.c',
    dependencies: sil6250_dep,
    install: false,
  )
endforeach
