# libsil6250 — userspace driver core for the SIL6250/Petaic fingerprint sensor.
#
# This is the standalone library that owns everything above the kernel broker:
# the mailbox framing, the TLS-PSK secure channel + capture engine, and the
# host-side matcher. The libfprint driver and the CLI tools are both thin
# consumers of this library (see ../fprint-driver and ../tools).

# Headers install under <prefix>/include/sil6250/ and are included flat
# ("petaic_proto.h"); the pkg-config file adds that dir to consumers' -I path.
sil6250_inc = include_directories('include/sil6250')
# transport.c speaks the kernel UAPI; the canonical header lives in ../kernel.
kernel_inc = include_directories('..' / 'kernel')

sil6250_headers = files(
  'include/sil6250/petaic_proto.h',
  'include/sil6250/petaic_transport.h',
  'include/sil6250/petaic_engine.h',
  'include/sil6250/petaic_match.h',
  'include/sil6250/petaic_sift.h',
)

libsil6250 = library(
  'sil6250',
  files(
    'src/petaic_proto.c',
    'src/petaic_transport.c',
    'src/petaic_engine.c',
    'src/petaic_match.c',
    'src/petaic_sift.c',
  ),
  include_directories: [sil6250_inc, kernel_inc],
  dependencies: [mbedtls_deps, m_dep],
  version: meson.project_version(),
  soversion: '0',
  install: true,
)

install_headers(sil6250_headers, subdir: 'sil6250')

# Public dependency for in-tree consumers (tools/). External consumers (the
# libfprint driver) use the installed pkg-config file generated below.
sil6250_dep = declare_dependency(
  link_with: libsil6250,
  include_directories: sil6250_inc,
  dependencies: [mbedtls_deps, m_dep],
)

# mbedTLS commonly ships no pkg-config file, so advertise the transitive link
# flags explicitly rather than via Requires.private.
pkgconfig.generate(
  libsil6250,
  name: 'libsil6250',
  description: 'SIL6250/Petaic fingerprint userspace driver (protocol, capture engine, matcher)',
  subdirs: 'sil6250',
  libraries_private: ['-lmbedtls', '-lmbedx509', '-lmbedcrypto', '-lm'],
)
