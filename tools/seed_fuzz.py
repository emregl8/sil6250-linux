# SPDX-License-Identifier: LGPL-2.1-or-later

from pathlib import Path
import struct

root = Path(__file__).resolve().parents[1] / 'fuzz/corpus'
for target in ['parsers', 'gallery', 'images']:
    (root / target).mkdir(parents=True, exist_ok=True)
feature = (b'SILF' + bytes([1, 0, 0, 0]) + struct.pack('<I', 1) +
           struct.pack('<5f', 1, 1, 1, 0, 1) + struct.pack('<128f', *([.25] * 128)))
(root / 'parsers/feature').write_bytes(feature)
(root / 'gallery/single').write_bytes(struct.pack('<I', len(feature)) + feature)
(root / 'gallery/empty-features').write_bytes(struct.pack('<I', 12) + b'SILF' + bytes([1, 0, 0, 0]) + bytes(4))
(root / 'images/pattern').write_bytes(bytes((i * 17 + (i // 64) * 31) % 256 for i in range(5120)))
