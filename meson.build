project(
  'sil6250',
  'c',
  version: '0.1.0',
  license: 'LGPL-2.1-or-later',
  meson_version: '>= 0.60.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

# Top-level build for the SIL6250/Petaic userspace stack:
#
#   lib/    libsil6250 — protocol, capture engine, host matcher (the core)
#   tools/  CLI utilities for bring-up, capture, and offline matcher ROC
#
# The kernel broker (kernel/) builds out-of-band via its own Makefile/DKMS, and
# the libfprint driver (fprint-driver/) is an overlay applied to an upstream
# libfprint checkout — see ./install.sh and the per-directory READMEs.

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

mbedtls_deps = [
  cc.find_library('mbedtls'),
  cc.find_library('mbedx509'),
  cc.find_library('mbedcrypto'),
]

pkgconfig = import('pkgconfig')

subdir('lib')

if get_option('tools')
  subdir('tools')
endif
