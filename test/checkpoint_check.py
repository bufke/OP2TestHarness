#!/usr/bin/env python3
"""Check of checkpoints (README "Checkpoints"): is a game resumed from a save in a new process the game that was saved,
going on? For a win script and checkpoint ticks T1,T2,...:
  1. the reference: a plain run of the script (`--eco --state`, telemetry every 10 ticks, trace every 20, `--trace-every`;
     all compared as streams, record by record: a long run's files are gigabytes);
  2. the checkpoint runs: the script with `T savegame` lines, which must play exactly like the reference (a save
     changes nothing), saving at each T (and its mission DLL state, `<save>.dll`);
  3. a resumed run per T: `run.py --resume` on that save with the script's lines after T, compared with the reference
     from T on: telemetry (heap addresses in the raw MapObject bytes masked, the hive slot numbers reported apart),
     trace (frame/anim aside), events after T (status messages apart), the final state.json and the end; and the
     harness's `checkpoint: dll` line (the mission DLL's memory outside its save region, after the load, against the
     saved game's).
With `--split`, each checkpoint run leaves out the script's lines at tick T and the resumed run plays them right after
the load (a session's orders given at a checkpoint's tick).
Needs Wine and Xvfb (sandbox disabled); saves go to the harness prefix's OPU/saves as ckptcheck<pid>_<T>.op2 and are
deleted after. About 3 runs' time per checkpoint.

    python3 harness/test/checkpoint_check.py --mission c_euf.dll --script harness/oracle/missions/c_euf_win.txt \\
        --at 10000,30000,45000 --out /tmp/ckcheck
"""
import argparse
import itertools
import resource
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))
import run  # noqa: E402


def script_lines(text: str) -> list:
    """(tick, coords mode, line) of each script line in harness order (stable by tick); comments and blanks dropped."""
    out, mode = [], "internal"
    for raw in text.splitlines():
        words = raw.split("#", 1)[0].split()
        if len(words) < 2:
            continue
        if words[1] == "coords":
            mode = "file" if len(words) > 2 and words[2] == "file" else "internal"
            continue
        out.append((int(words[0]), mode, " ".join(words)))
    out.sort(key=lambda t: t[0])
    return out


def write_script(path: Path, lines: list) -> None:
    text, mode = [], None
    for _, m, line in lines:
        if m != mode:
            text.append(f"0 coords {m}")
            mode = m
        text.append(line)
    path.write_text("\n".join(text) + "\n")


def harness(args, script: Path, out: Path, ticks: int, resume: str | None = None) -> dict:
    cmd = [sys.executable, str(HERE / "run.py"), "--mission", args.mission, "--script", str(script), "--ticks", str(ticks),
           "--eco", "--state", "--telemetry-every", "10", "--trace-every", str(args.trace_every), "--out", str(out)]
    if args.campaign:
        cmd += ["--campaign", str(args.campaign)]
    if args.players > 1:
        cmd += ["--players", str(args.players)]
    if resume:
        cmd += ["--resume", resume]
    p = subprocess.run(cmd, capture_output=True, text=True)
    summary = next((json.loads(l) for l in reversed(p.stdout.splitlines()) if l.startswith("{")), None)
    assert summary and not summary["timed_out"], p.stdout + p.stderr
    return summary


def records(path: Path, since: int = 0, until: int | None = None):
    """The JSON lines of a file with a tick in since..until, one at a time: a trace or an `--eco` telemetry of a long
    run is gigabytes, never read whole (an earlier version that did ran out of memory). Lines are in tick order."""
    if not path.exists():
        return
    with path.open() as f:
        for line in f:
            if line.startswith('{"tick":'):
                end = line.find(",", 8)
                if end > 8 and line[8:end].isdigit():
                    tick = int(line[8:end])
                    if until is not None and tick > until:
                        return
                    if tick < since:
                        continue
            try:
                j = json.loads(line)
            except json.JSONDecodeError:
                continue
            if j.get("tick", 0) >= since and (until is None or j.get("tick", 0) <= until):
                yield j


def last_tick(path: Path) -> int:
    tick = -1
    for j in records(path):
        tick = j.get("tick", tick)
    return tick


def is_pointer(v: int) -> bool:
    return 0x01000000 <= v < 0x80000000


def mask_pair(a: dict, b: dict) -> None:
    """Takes out of both telemetry records what a save or a load in a new process legitimately changes: in each unit's
    raw MapObject bytes, +0x7C (a heap address for most units, record_check.py masks it too; a Garage's goes from -2 to
    0x40 when the game is saved) and any other dword that differs where both values are heap addresses (a structure's
    +0x60..+0x68 after a load in a new process, 2 MB apart); and the structure hives' slot numbers (HiveManager's
    array; moved to their own key, `hiveSlot`)."""
    ub = {u["id"]: u for u in b.get("units", [])}
    for u in a.get("units", []):
        v = ub.get(u["id"])
        if v and "raw" in u and "raw" in v and u["raw"] != v["raw"] and len(u["raw"]) == len(v["raw"]):
            ra, rb = bytearray.fromhex(u["raw"]), bytearray.fromhex(v["raw"])
            for k in range(0, len(ra) - 3, 4):
                x, y = int.from_bytes(ra[k:k + 4], "little"), int.from_bytes(rb[k:k + 4], "little")
                if x != y and ((is_pointer(x) and is_pointer(y)) or k == 0x7C - 0x48):
                    ra[k:k + 4] = rb[k:k + 4] = bytes(4)
            u["raw"], v["raw"] = ra.hex(), rb.hex()
    for t in (a, b):
        for w in t.get("units", []):
            if "hive" in w:
                w["hiveSlot"] = w["hive"][-1]
                w["hive"] = w["hive"][:-1]


def diff_keys(a, b, path: str = "") -> list:
    """Where two JSON values differ: `units[id].field (a / b)`; lists of objects with ids are matched by id."""
    if isinstance(a, dict) and isinstance(b, dict):
        out = []
        for k in sorted(set(a) | set(b)):
            if a.get(k) != b.get(k):
                out += diff_keys(a.get(k), b.get(k), f"{path}.{k}" if path else k)
        return out
    if isinstance(a, list) and isinstance(b, list) and a + b and all(isinstance(x, dict) and "id" in x for x in a + b):
        da, db = {x["id"]: x for x in a}, {x["id"]: x for x in b}
        return [p for i in sorted(set(da) | set(db)) if da.get(i) != db.get(i) for p in diff_keys(da.get(i), db.get(i), f"{path}[{i}]")]
    return [f"{path} ({json.dumps(a)[:70]} / {json.dumps(b)[:70]})"]


def compare_rows(name: str, ref, got, telemetry: bool = False, apart: tuple = ()) -> tuple:
    """(verdict text, ok): each differing field with the ticks it differs over. A field whose path contains one of
    `apart` is listed but doesn't fail. `ref` and `got` are iterables, walked in step (only the spans are kept)."""
    spans = {}
    n_ref = n_got = 0
    for a, b in itertools.zip_longest(ref, got):
        n_ref += a is not None
        n_got += b is not None
        if a is None or b is None or a == b:
            continue
        if telemetry:
            mask_pair(a, b)
        for k in diff_keys(a, b):
            key = k.split(" (", 1)[0]
            first, _, n, example = spans.get(key, (a.get("tick"), 0, 0, k))
            spans[key] = (first, a.get("tick"), n + 1, example)
    failing = [k for k in spans if not any(x in k for x in apart)]
    ok = not failing and n_ref == n_got
    verdict = "identical" if not spans and ok else "DIFFERS" if not ok else f"equal apart from {len(spans)} reported fields"
    lines = [f"{name} {verdict} ({n_ref} records)"]
    # The fields that fail first, then by the tick they first differ.
    for k, (first, last, n, example) in sorted(spans.items(), key=lambda kv: (kv[0] not in failing, kv[1][0]))[:12]:
        lines.append(f"    {example}: ticks {first}-{last} ({n} records){'' if k in failing else ' [apart]'}")
    if len(spans) > 12:
        lines.append(f"    ... {len(spans) - 12} more fields")
    if n_ref != n_got:
        lines.append(f"    {n_ref} records against {n_got}")
    return "\n".join(lines), ok


def messages(name: str, ref: list, got: list) -> str:
    """Status messages (UI, not game state) as (tick, text) sets: those only one side has."""
    a = {(e["tick"], e["text"]) for e in ref}
    b = {(e["tick"], e["text"]) for e in got}
    only = sorted(a - b)[:3], sorted(b - a)[:3]
    return f"{name}: {len(a)} / {len(b)}, {len(a - b)} only in the reference {only[0]}, {len(b - a)} only resumed {only[1]}"


def trace_rows(d: Path, since: int, until: int | None = None):
    """The trace without frame/anim (what the renderer last drew), one record at a time; none with --trace-every 0."""
    for t in records(d / "trace.jsonl", since, until):
        for u in t.get("units", []):
            u.pop("frame", None)
            u.pop("anim", None)
        yield t


def dll_line(d: Path) -> str:
    log = (d / "harness.log").read_text(errors="replace")
    m = re.search(r"^checkpoint: dll .*$", log, re.M)
    return m.group(0) if m else "checkpoint: (no dll line)"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mission", required=True)
    ap.add_argument("--campaign", type=Path)
    ap.add_argument("--players", type=int, default=1, help="a multiplayer start's human players (run.py --players)")
    ap.add_argument("--script", type=Path, required=True)
    ap.add_argument("--at", required=True, help="checkpoint ticks, comma separated")
    ap.add_argument("--ticks", type=int, default=0, help="play to this tick (0: the mission's end)")
    ap.add_argument("--trace-every", type=int, default=20,
                    help="trace interval (default 20: a long run's trace at 4 is gigabytes; 0 = no trace)")
    ap.add_argument("--split", action="store_true", help="the lines at each checkpoint's tick run after the load")
    ap.add_argument("--keep", action="store_true", help="keep the saves (and their .dll files) in OPU/saves")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    lines = script_lines(args.script.read_text())
    ats = sorted(int(t) for t in args.at.split(","))
    ref = out / "ref"
    write_script(out / "script.txt", lines)
    s = harness(args, out / "script.txt", ref, args.ticks)
    end_tick = last_tick(ref / "telemetry.jsonl")
    print(f"reference: {s['quit']} ({s['game_term_reason']})")
    ok = True
    saves = []
    try:
        groups = [[t] for t in ats] if args.split else [ats]
        for group in groups:
            names = {t: f"ckptcheck{run.os.getpid()}_{t}.op2" for t in group}
            saves += names.values()
            kept = [l for l in lines if not (args.split and l[0] == group[0])]
            saving = sorted(kept + [(t, "internal", f"{t} savegame {names[t]}") for t in group], key=lambda l: l[0])
            write_script(out / "save.txt", saving)
            sd = out / f"save_{group[0]}"
            harness(args, out / "save.txt", sd, max(group))
            log = (sd / "harness.log").read_text(errors="replace")
            missing = [t for t in group if f"{names[t]} at tick {t}" not in log or not (run.OPU_DIR / "saves" / names[t]).exists()]
            assert not missing, f"no save at {missing}: {sd / 'harness.log'}"
            if not args.split:
                end = max(group)
                for what, a, b in (("telemetry", records(ref / "telemetry.jsonl", 0, end), records(sd / "telemetry.jsonl", 0, end)),
                                   ("trace", trace_rows(ref, 0, end), trace_rows(sd, 0, end))):
                    text, good = compare_rows(f"  with saves at {','.join(map(str, group))}: {what} to {max(group)}", a, b,
                                              what == "telemetry", ("hiveSlot",))
                    print(text)
                    ok &= good
            for t in group:
                rest = [l for l in lines if l[0] > t or (args.split and l[0] == t)]
                write_script(out / f"rest_{t}.txt", rest)
                rd = out / f"resume_{t}"
                s = harness(args, out / f"rest_{t}.txt", rd, args.ticks, resume=names[t])
                print(f"resume at {t}: {s['quit']} ({s['game_term_reason']}); {dll_line(rd)}")
                if not (rd / "state.json").is_file():
                    log = (rd / "harness.log").read_text(errors="replace").splitlines()
                    print(f"  FAILED: no state.json; harness.log ends: {log[-3:]}")
                    ok = False
                    continue
                # Events are small (thousands of lines): read whole.
                ev_ref, ev_got = list(records(ref / "events.jsonl", t + 1)), list(records(rd / "events.jsonl", t + 1))
                results = [
                    # The record at the checkpoint's tick itself is taken right after the load, before the game's next
                    # cycle recomputes what a load leaves to it (each player's recalc flag, the structures' hive counters).
                    compare_rows(f"  telemetry at {t}, right after the load", records(ref / "telemetry.jsonl", t, t),
                                 records(rd / "telemetry.jsonl", t, t), True, ("",)),
                    compare_rows("  telemetry after it", records(ref / "telemetry.jsonl", t + 1), records(rd / "telemetry.jsonl", t + 1), True, ("hiveSlot",)),
                    compare_rows("  trace", trace_rows(ref, t), trace_rows(rd, t)),
                    compare_rows("  events after it (status messages apart)", [e for e in ev_ref if e["ev"] != "msg"],
                                 [e for e in ev_got if e["ev"] != "msg"]),
                ]
                sa, sb = json.loads((ref / "state.json").read_text()), json.loads((rd / "state.json").read_text())
                results.append((f"  final state.json {'identical' if sa == sb else 'DIFFERS: ' + '; '.join(diff_keys(sa, sb)[:4])}", sa == sb))
                for text, good in results:
                    print(text)
                    ok &= good
                print(messages("  status messages after it", [e for e in ev_ref if e["ev"] == "msg"], [e for e in ev_got if e["ev"] == "msg"]))
                if args.split:
                    at_t = list(records(rd / "events.jsonl", t, t))
                    ref_t = list(records(ref / "events.jsonl", t, t))
                    print(f"  events at {t}: resumed {len(at_t)}, reference {len(ref_t)}")
                tail = last_tick(rd / "telemetry.jsonl")
                if tail != end_tick:
                    print(f"  the resumed run ended at {tail}, the reference at {end_tick}")
                    ok = False
    finally:
        for n in [] if args.keep else saves:
            for f in (n, n + ".dll", n + ".hive"):
                (run.OPU_DIR / "saves" / f).unlink(missing_ok=True)
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024
    print(f"checkpoint check: {'PASS' if ok else 'FAIL'} (peak RSS {peak} MB)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
