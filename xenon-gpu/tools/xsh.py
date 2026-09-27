"""Read Xenia/ReXGlue shader storage files (.xsh): the runtime-recorded shader corpus."""
import struct
from pathlib import Path

MAGIC = 0x48534558  # 'XESH'


def read_xsh(path):
    """Yield (hash, type, ucode_le_bytes); type 0 = vertex, 1 = pixel."""
    d = Path(path).read_bytes()
    magic, _version = struct.unpack("<II", d[:8])
    if magic != MAGIC:
        raise ValueError(f"{path}: not a shader storage file")
    pos = 8
    while pos + 12 <= len(d):
        h, bits = struct.unpack("<QI", d[pos:pos + 12])
        count, typ = bits & 0x7FFFFFFF, bits >> 31
        pos += 12
        yield h, typ, d[pos:pos + count * 4]
        pos += count * 4
