#!/usr/bin/env python3
"""Decrypt + decompress a retail XEX2 (normal AES, none/basic compression) to a raw image.

Returns (load_address, image_bytes). LZX ("normal") compression is not handled;
use XenonUtils for those.
"""

import struct
import subprocess
import sys
from pathlib import Path

XEX2_RETAIL_KEY = "20B185A59D28FDC340583FBB0896BF91"
IV = "00" * 16


def _aes_cbc_decrypt(key_hex, data):
    return subprocess.run(
        ["openssl", "enc", "-d", "-aes-128-cbc", "-nopad", "-K", key_hex, "-iv", IV],
        input=data, stdout=subprocess.PIPE, check=True).stdout


def load(path):
    d = Path(path).read_bytes()
    magic, _flags, header_size, _res, sec_off, nopt = struct.unpack(">4sIIIII", d[:24])
    if magic != b"XEX2":
        raise ValueError(f"{path}: not XEX2")
    opts = dict(struct.unpack(">II", d[24 + i * 8:32 + i * 8]) for i in range(nopt))
    image_size = struct.unpack(">I", d[sec_off + 4:sec_off + 8])[0]
    load_addr = struct.unpack(">I", d[sec_off + 0x110:sec_off + 0x114])[0]
    aes_key = d[sec_off + 0x150:sec_off + 0x160]

    ffi = opts[0x3FF]
    info_size, enc, comp = struct.unpack(">IHH", d[ffi:ffi + 8])
    src = d[header_size:]
    if enc == 1:
        session_key = _aes_cbc_decrypt(XEX2_RETAIL_KEY, aes_key).hex()
        src = _aes_cbc_decrypt(session_key, src[:len(src) & ~15])

    if comp == 0:
        img = src[:image_size]
    elif comp == 1:
        out, pos = bytearray(), 0
        for i in range((info_size - 8) // 8):
            data_size, zero_size = struct.unpack(">II", d[ffi + 8 + i * 8:ffi + 16 + i * 8])
            out += src[pos:pos + data_size] + bytes(zero_size)
            pos += data_size
        img = bytes(out)
    else:
        raise NotImplementedError(f"{path}: compression type {comp} (LZX) not supported")
    return load_addr, img


if __name__ == "__main__":
    base, img = load(sys.argv[1])
    print(f"load=0x{base:08X} size=0x{len(img):X}")
    if len(sys.argv) > 2:
        Path(sys.argv[2]).write_bytes(img)
