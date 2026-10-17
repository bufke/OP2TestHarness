#!/usr/bin/env python3
"""Outpost2.exe chat cheat hash (DSEG 0x58642A), for checking candidate phrases (notes/findings-cheats.md).

usage: cheat_hash.py "Dan's dog could bite harder!" ...   prints the hash and which cheat it triggers, if any.
"""
import struct
import sys

CHEATS = {0xFF27BC04: "dog: ore to max", 0x48D60158: "car: +25% workers/scientists",
          0xAC0FF0E5: "bike: weapon reload", 0xABE0CC11: "bite: weapon penetration damage"}


def rol(x, n):
    n &= 31
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF if n else x


def cheat_hash(text: str) -> int:
    b = text.encode("latin1") + b"\0" * 8
    h = 0
    cur = prev = struct.unpack_from("<I", b, 0)[0]   # the first dword is also its own "previous" dword
    i = 0
    while True:
        shift = (prev & 0xFF) >> 4
        for k in range(4):
            h = rol(h, shift) ^ prev                  # mixes the PREVIOUS dword...
            if (cur >> (8 * k)) & 0xFF == 0:          # ...but stops at the first NUL of the CURRENT one
                return h
        h ^= cur >> 24
        prev = cur
        i += 4
        cur = struct.unpack_from("<I", b, i)[0]


if __name__ == "__main__":
    for s in sys.argv[1:]:
        v = cheat_hash(s)
        print(f"{v:#010x}  {CHEATS.get(v, '-'):32}  {s!r}")
