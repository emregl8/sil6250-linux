import os
from pathlib import Path
import shlex
import subprocess
import struct


LIBRARIES = {'libfprint-2.so.2.0.0': 'libfprint-2.so.2',
             'libsil6250_ffi.so.0': 'libsil6250_ffi.so.0'}


def build_library(directory, name, defect=None):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    if defect == 'non-elf':
        path.write_bytes(b'not an ELF library')
        return
    source = '#include <stdio.h>\nint fixture(void) { return puts("fixture"); }\n'
    flags = []
    soname = LIBRARIES[name]
    if defect == 'soname':
        soname = 'libwrong.so.99'
    elif defect in {'runpath', 'rpath'}:
        flags += ['-Wl,-rpath,/tmp/untrusted',
                  '-Wl,--enable-new-dtags' if defect == 'runpath' else '-Wl,--disable-new-dtags']
    elif defect == 'asan':
        flags += ['-fsanitize=address']
    elif defect == 'sanitizer-symbol':
        source = ('extern void __ubsan_handle_fixture(void);\n'
                  'int fixture(void) { __ubsan_handle_fixture(); return 0; }\n')
    elif defect == 'elf32':
        source = 'int fixture(void) { return 0; }\n'
        flags += ['-m32', '-nostdlib']
    elif defect == 'pie':
        source = 'int fixture(void) { return 0; }\n'
        flags += ['-nostdlib', '-pie']
    elif defect not in {None, 'machine', 'byte-order', 'executable', 'truncated'}:
        raise ValueError(defect)
    mode = [] if defect == 'pie' else ['-shared']
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), *mode, '-fPIC',
                    '-x', 'c', '-', '-Wl,-soname,' + soname, *flags, '-o', str(path)],
                   input=source, text=True, check=True, capture_output=True)
    if defect in {'machine', 'byte-order', 'executable', 'truncated'}:
        data = bytearray(path.read_bytes())
        if defect == 'machine':
            machine = struct.unpack_from('<H' if data[5] == 1 else '>H', data, 18)[0]
            struct.pack_into('<H' if data[5] == 1 else '>H', data, 18, 183 if machine != 183 else 62)
        elif defect == 'byte-order':
            data[5] = 2 if data[5] == 1 else 1
        elif defect == 'executable':
            struct.pack_into('<H' if data[5] == 1 else '>H', data, 16, 2)
        else:
            del data[20:]
        path.write_bytes(data)


def build_libraries(directory):
    for name in LIBRARIES:
        build_library(directory, name)
