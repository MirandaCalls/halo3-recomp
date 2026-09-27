"""Minimal Xenos microcode control-flow parser: program length and shader type.

Control flow instructions are 48 bits, packed two per 3-dword slot at the start
of the program. Exec-type instructions reference `count` ALU/fetch slots
starting at slot `address`, so the program ends at max(address + count) slots.
alloc instructions reveal the stage: vertex shaders allocate a position export.
"""
import struct

EXECS = {1, 2, 3, 4, 5, 6, 13, 14}
EXEC_ENDS = {2, 4, 6, 14}
VALID = set(range(16))
MAX_SLOTS = 4096


def unpack_pair(w0, w1, w2):
    a = (w0, w1 & 0xFFFF)
    b = (((w1 >> 16) | (w2 << 16)) & 0xFFFFFFFF, w2 >> 16)
    return a, b


def parse(words, max_slots=MAX_SLOTS):
    """words: host-order dwords (already converted from guest big-endian).

    Returns (length_in_dwords, stage) with stage 'vs'/'ps'/'?' or None if the
    data is not a plausible program."""
    cf_end = None  # first slot holding ALU/fetch instructions
    end_slot = 0
    seen_end = False
    allocs = set()
    slot = 0
    n_cf = 0
    while True:
        if cf_end is not None and slot >= cf_end:
            break
        if slot >= max_slots or (slot + 1) * 3 > len(words):
            return None
        for d0, d1 in unpack_pair(*words[slot * 3:slot * 3 + 3]):
            op = (d1 >> 12) & 0xF
            n_cf += 1
            if op in EXECS:
                addr, count = d0 & 0xFFF, (d0 >> 12) & 0x7
                if count == 0 and op not in EXEC_ENDS:
                    return None
                if addr == 0 or addr > max_slots:
                    return None
                cf_end = addr if cf_end is None else min(cf_end, addr)
                end_slot = max(end_slot, addr + count)
                if op in EXEC_ENDS:
                    seen_end = True
            elif op == 12:  # alloc
                allocs.add((d1 >> 9) & 0x3)
            elif op == 0:  # nop
                pass
            elif op in (7, 8, 9, 10, 11, 15):
                pass
        slot += 1
        if seen_end and cf_end is None:
            return None
        if seen_end and slot >= (cf_end or 0):
            break
        if n_cf > 2 * max_slots:
            return None
    if not seen_end or cf_end is None or end_slot <= cf_end:
        return None
    length = end_slot * 3
    if length > len(words):
        return None
    stage = "vs" if 1 in allocs else ("ps" if allocs else "?")
    return length, stage


def words_be(data):
    return list(struct.unpack(f">{len(data) // 4}I", data[:len(data) // 4 * 4]))
