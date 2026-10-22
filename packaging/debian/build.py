#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def output(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-root', type=Path, default=repo / 'build/native')
    parser.add_argument('--output', type=Path, default=repo / 'build/packages')
    parser.add_argument('--version', default='0.3.0~dev1')
    parser.add_argument('--maintainer')
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error('Build packages as your regular user')
    if not re.fullmatch(r'[0-9][A-Za-z0-9.+~:-]*', args.version):
        parser.error('Invalid Debian version')
    maintainer = args.maintainer
    if maintainer is None:
        name = output('git', '-C', str(repo), 'config', 'user.name')
        email = output('git', '-C', str(repo), 'config', 'user.email')
        maintainer = f'{name} <{email}>'
    if '\n' in maintainer or '\r' in maintainer or not re.fullmatch(r'[^<>]+ <[^<>\s]+@[^<>\s]+>', maintainer):
        parser.error('Set a valid --maintainer "Name <email>"')
    arch = output('dpkg-architecture', '-qDEB_HOST_ARCH')
    triplet = output('dpkg-architecture', '-qDEB_HOST_MULTIARCH')
    package = 'sil6250-libfprint'
    args.output.mkdir(parents=True, exist_ok=True)
    destination = args.output.resolve() / f'{package}_{args.version}_{arch}.deb'
    if destination.exists():
        parser.error('Output already exists; choose a new version or output directory')
    with tempfile.TemporaryDirectory(prefix='.sil6250-package-', dir=args.output.resolve()) as temp:
        root = Path(temp)
        debian = root / 'debian'
        debian.mkdir()
        stage = debian / package
        library_path = Path('usr/lib') / triplet / 'sil6250'
        libraries = stage / library_path
        libraries.mkdir(parents=True)
        names = ['libfprint-2.so.2.0.0', 'libsil6250_ffi.so.0']
        for name in names:
            source = args.build_root / 'prefix/lib' / name
            dynamic = output('readelf', '-d', str(source))
            if re.search(r'\((?:RUNPATH|RPATH)\)', dynamic):
                raise RuntimeError('Runtime library search paths must be absent: ' + name)
            shutil.copyfile(source, libraries / name)
            (libraries / name).chmod(0o644)
        (libraries / 'libfprint-2.so.2').symlink_to('libfprint-2.so.2.0.0')
        dropin = stage / 'usr/lib/systemd/system/fprintd.service.d/60-sil6250.conf'
        dropin.parent.mkdir(parents=True)
        dropin.write_text('[Service]\nEnvironment=LD_LIBRARY_PATH=/' + str(library_path) +
                          '\nDeviceAllow=/dev/sil6250 rw\nUMask=0077\n')
        docs = stage / 'usr/share/doc' / package
        docs.mkdir(parents=True)
        shutil.copyfile(repo / 'LICENSE', docs / 'LICENSE')
        (docs / 'NOTICE').write_text(
            'SIL6250 core sources retain GPL-2.0 notices; the C interface and\n'
            'libfprint driver are LGPL-2.1-or-later. Root LICENSE contains LGPL-2.1.\n'
            'Dependencies retain their respective licenses. Combined-binary\n'
            'redistribution permissions require clarification; this development\n'
            'package does not grant a license exception. See repository README.\n')
        (docs / 'build-info').write_text('libfprint=' + (repo / 'integrations/libfprint/revision').read_text() +
            ''.join(hashlib.sha256((libraries / name).read_bytes()).hexdigest() + '  ' + name + '\n'
                    for name in names))
        (debian / 'control').write_text('Source: ' + package + '\n\nPackage: ' + package +
                                       '\nArchitecture: any\nDescription: SIL6250 libfprint driver\n')
        (debian / 'shlibs.local').write_text('libsil6250_ffi 0 ' + package + ' (= ' + args.version + ')\n')
        dependencies = output('dpkg-shlibdeps', '-O', '--package=' + package, '-x' + package, '-l' + str(libraries),
                              *[str(libraries / name) for name in names], cwd=root)
        if not dependencies.startswith('shlibs:Depends='):
            raise RuntimeError('Could not determine runtime dependencies')
        metadata = stage / 'DEBIAN'
        metadata.mkdir()
        (metadata / 'control').write_text(
            f'Package: {package}\nVersion: {args.version}\nArchitecture: {arch}\n'
            f'Maintainer: {maintainer}\nSection: admin\nPriority: optional\n'
            'Depends: fprintd (>= 1.94), ' + dependencies.split('=', 1)[1] + '\n'
            'Suggests: sil6250-dkms\nConflicts: open-fprintd, sil6250d\n'
            'Description: Native SIL6250 driver for standard fprintd\n'
            ' Private libfprint runtime and SIL6250 capture engine.\n'
            ' Existing enrollment records and PAM configuration are preserved.\n')
        script = ('#!/bin/sh\nset -eu\n'
                  'if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then\n'
                  '    systemctl daemon-reload\nfi\n')
        for name in ['postinst', 'postrm']:
            (metadata / name).write_text(script)
            (metadata / name).chmod(0o755)
        for path in [stage, *stage.rglob('*')]:
            if not path.is_symlink():
                executable = path.parent == metadata and path.name in {'postinst', 'postrm'}
                path.chmod(0o755 if path.is_dir() or executable else 0o644)
        env = os.environ.copy()
        env.setdefault('SOURCE_DATE_EPOCH', output('git', '-C', str(repo), 'show', '-s', '--format=%ct', 'HEAD'))
        archive = root / 'result.deb'
        subprocess.run(['dpkg-deb', '--root-owner-group', '-Zxz', '--build', str(stage), str(archive)],
                       check=True, env=env)
        os.link(archive, destination)
    print(destination)


if __name__ == '__main__':
    main()
