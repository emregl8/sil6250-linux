import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys


def elf_header(path):
    with path.open('rb') as stream:
        header = stream.read(64)
    if len(header) < 20 or header[:4] != b'\x7fELF' or header[4] not in {1, 2} or header[5] not in {1, 2}:
        raise RuntimeError('Invalid ELF header: ' + str(path))
    expected_size = 52 if header[4] == 1 else 64
    if len(header) < expected_size or header[6] != 1:
        raise RuntimeError('Truncated or unsupported ELF header: ' + str(path))
    file_type, machine = struct.unpack_from('<HH' if header[5] == 1 else '>HH', header, 16)
    return (header[4], header[5], machine), file_type


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('libdir', type=Path)
    parser.add_argument('--reference', type=Path, default=Path(sys.executable))
    args = parser.parse_args()
    target, _ = elf_header(args.reference)
    for name, soname in [('libfprint-2.so.2.0.0', 'libfprint-2.so.2'),
                         ('libsil6250_ffi.so.0', 'libsil6250_ffi.so.0')]:
        path = args.libdir / name
        architecture, file_type = elf_header(path)
        if architecture != target:
            raise RuntimeError('ELF architecture mismatch: ' + name)
        if file_type != 3:
            raise RuntimeError('Expected an ELF shared object: ' + name)
        dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True)
        if re.search(r'\(FLAGS_1\).*\bPIE\b', dynamic):
            raise RuntimeError('Executable supplied as a runtime library: ' + name)
        if not re.search(r'\(SONAME\).*\[' + re.escape(soname) + r'\]', dynamic):
            raise RuntimeError('Unexpected SONAME: ' + name)
        if re.search(r'\((?:RUNPATH|RPATH)\)', dynamic):
            raise RuntimeError('Embedded runtime search path: ' + name)
        if re.search(r'\(NEEDED\).*\[lib(?:asan|ubsan|tsan|lsan|clang_rt\.)', dynamic):
            raise RuntimeError('Sanitizer dependency in production library: ' + name)
        symbols = subprocess.check_output(['readelf', '--dyn-syms', '--wide', str(path)], text=True)
        if re.search(r'\b__(?:asan|ubsan|tsan|lsan|sanitizer)_', symbols):
            raise RuntimeError('Sanitizer symbols in production library: ' + name)
        print(name + ': runtime checks passed')


if __name__ == '__main__':
    main()
