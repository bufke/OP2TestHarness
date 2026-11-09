#!/usr/bin/env python3
"""Turns a harness script's order verbs into `packet` lines: the same orders as the command packets the game's UI sends
(README.md "Recording your own play"), so a verb script can stand in for a recording of a person's play, e.g. to
check op2-rs's reading of every packet type against the original. The bytes are the ones the verbs build (harness.cpp
MovePacket, WaypointPacket, TargetedPacket and the structure and earthwork packets).

    python3 tools/packetize.py $ORACLE/missions/e01.txt > $ORACLE/missions/e01_packets.txt

A harness verb at tick T runs after cycle T, so its order acts in cycle T+1; a packet at tick P acts in cycle P, and
only in a cycle that is a multiple of 4, one packet each. Each order goes to the first such cycle at or after T+1 and
after the previous order's cycle (orders keep their script order). Lines it can't turn into a packet are kept as they
are: other verbs, units named by `@N` or `$NAME`, a `produce` or `research` without --gamedata (map ids and tree indices
come from the game's tables), `build` and `route` (a kit's footprint, a dock's place). `coords file` needs --pad N (32 on a
non-wrapping map).
"""
import argparse
import json
import re
import struct
import sys

WALLS = {"TUBE": 0x11, "WALL": 0x12, "WALL_LAVA": 0x13, "WALL_MICROBE": 0x14}
TYPES = {"doze": 1, "move": 2, "dock": 3, "dockeg": 4, "stop": 5, "buildwall": 7, "removewall": 8, "transfercargo": 10,
         "load": 11, "unload": 12, "idle": 18, "unidle": 19, "selfdestruct": 20, "scatter": 21, "train": 23, "launch": 25,
         "repairobj": 28, "reprogram": 29, "dismantle": 30, "lights": 35, "attack": 36, "guard": 37,
         "standground": 38, "patrol": 40, "movewp": 2, "attackground": 36}


def u16(*v):
    return b"".join(struct.pack("<H", x & 0xFFFF) for x in v)


def waypoint(x, y, rng=0):
    """A tile's centre as the packet's word: x in bits 0..14, y in 15..28, the range in the top three."""
    return struct.pack("<I", (x * 32 + 16) | (y * 32 + 16) << 15 | (rng & 7) << 29)


def selection(ids):
    return bytes([len(ids)]) + u16(*ids)


def packet(verb, a, pad, game=None):
    """(type, data) for the verb's words after the tick, or None. `game` is a run's gamedata.json (for produce and research)."""
    num = lambda i: int(a[i], 0)
    if any(w.startswith(("@", "$")) for w in a) or "as" in a:
        return None
    t = TYPES.get(verb)
    if verb == "produce" and game:
        ids = {t["code"].upper(): t["mapId"] for t in game["unitTypes"]}
        if a[1].upper() not in ids:
            return None
        weapon = ids.get(a[2].upper(), 0) if len(a) > 2 else 0
        return 9, u16(num(0), ids[a[1].upper()], weapon, 0xFFFF)
    if verb == "research" and game:
        index = next((i for i, t in enumerate(game["techTree"]) if t[0] == num(1)), None)
        return (22, u16(num(0), index, num(2))) if index is not None else None
    if verb in ("move", "dock", "dockeg"):
        return t, selection([num(0)]) + u16(1) + waypoint(num(1) + pad, num(2))
    if verb in ("movewp", "standground", "patrol"):
        first = 2 if verb == "patrol" else 1
        pts = []
        for w in a[first:]:
            m = re.match(r"(-?\d+),(-?\d+)(?:,(\d+))?", w)
            if m:
                pts.append(waypoint(int(m[1]) + pad, int(m[2]), int(m[3] or 0)))
        if not pts or len(pts) > 8:
            return None
        data = selection([num(0)]) + u16(len(pts)) + b"".join(pts)
        return t, data + (u16(num(1)) if verb == "patrol" else b"")
    if verb in ("stop", "selfdestruct", "scatter"):
        return t, selection([num(i) for i in range(len(a))])
    if verb == "lights":
        return t, selection([num(0)]) + u16(num(1))
    if verb in ("attack", "guard"):
        return t, selection([num(0)]) + u16(0, num(1), 0xFFFF)
    if verb == "attackground":
        return t, selection([num(0)]) + u16(0, num(1) + pad, num(2))
    if verb in ("load", "unload", "idle", "unidle"):
        return t, u16(num(0))
    if verb == "train":
        return t, u16(num(0), num(1))
    if verb == "transfercargo":
        return t, u16(num(0), num(1), num(2) if len(a) > 2 else 0)
    if verb == "launch":
        return t, u16(num(0), *((num(1), num(2)) if len(a) > 2 else (0, 0)))
    if verb in ("repairobj", "reprogram", "dismantle"):
        ids = [num(0)] + [int(w) for w in a[2:]]
        target = 0 if a[1] == "none" else num(1)
        return t, selection(ids) + u16(0, target, 0xFFFF)
    if verb == "doze":
        return t, selection([num(0)]) + u16(num(1) + pad, num(2), num(3) + pad, num(4))
    if verb == "buildwall":
        wall = WALLS.get(a[5].upper(), None) if len(a) > 5 else 0x11
        if wall is None:
            return None
        return t, selection([num(0)]) + u16(num(1) + pad, num(2), num(3) + pad + 1, num(4) + 1, wall, 0)
    if verb == "removewall":
        x, y = num(1) + pad, num(2)
        return t, selection([num(0)]) + u16(1) + waypoint(x, y) + u16(x, y, x, y)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("script")
    ap.add_argument("--pad", type=int, default=32, help="tiles from file x to internal x, for `coords file` (default 32)")
    ap.add_argument("--interval", type=int, default=4)
    ap.add_argument("--max-delay", type=int, help="keep as a verb an order that would run more than N ticks after its tick "
                    "(a crowded tick pushes orders later; a structure that is built or lost meanwhile gets a packet it can't take)")
    ap.add_argument("--keep", default="", help="verbs (comma separated) to leave as they are, e.g. attack: the verb sends nothing "
                    "to a target that isn't live, a packet does, and a delayed order may find its target gone")
    ap.add_argument("--gamedata", help="a run's gamedata.json: turns `produce` and `research` into packets too")
    args = ap.parse_args()
    pad = 0
    game = json.load(open(args.gamedata)) if args.gamedata else None
    lines = open(args.script).read().splitlines()
    parsed = []
    for line in lines:
        body = line.split("#")[0].split()
        parsed.append((int(body[0]), body[1], body[2:]) if len(body) >= 2 and body[0].isdigit() else None)
    # `coords file` applies to the lines after it in the file; the script itself runs in tick order (stable).
    packets = {}
    for n, item in enumerate(parsed):
        if item is None:
            continue
        tick, verb, a = item
        if verb == "coords":
            pad = args.pad if a[:1] == ["file"] else 0
        try:
            p = None if verb in args.keep.split(",") else packet(verb, a, pad, game)
        except (ValueError, IndexError):
            p = None
        if p is not None:
            packets[n] = (tick, p)
    out, last = list(lines), 0
    for n in sorted(packets, key=lambda n: packets[n][0]):
        tick, (ty, data) = packets[n]
        cycle = max(tick + 1, last + args.interval)
        cycle += -cycle % args.interval
        if args.max_delay is not None and cycle - tick > args.max_delay:
            continue
        last = cycle
        was = lines[n].split("#")[0].split()
        comment = lines[n].split("#", 1)[1].strip() if "#" in lines[n] else ""
        out[n] = f"{cycle} packet {ty} {data.hex()}  # was: {' '.join(was)}" + (f"  # {comment}" if comment else "")
    converted = sum(1 for l in out if " packet " in l.split("#")[0])
    print(f"# Generated by tools/packetize.py from {args.script}: {converted} orders as command packets.")
    # The source's comment-only lines describe its verbs (and name its own run); they don't carry over.
    print("\n".join(l for n, l in enumerate(out) if parsed[n] is not None or l.strip() and not l.lstrip().startswith("#")))
    print(f"{converted} orders turned into packets", file=sys.stderr)


if __name__ == "__main__":
    main()
