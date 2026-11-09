"""Compares two harness traces (trace.jsonl) field by field, tick by tick: which fields agree, and where each first differs.

  python -I windows/trace_diff.py A/trace.jsonl B/trace.jsonl [--labels original,port] [--json report.json]

Units are matched by spawn index (the scenario's Nth spawn). For each spawn and field the report gives the first tick
the two disagree and both values there; a spawn present in one trace and not the other (died, or never made) counts as
the field `present`. The RNG state is compared per tick as `rng`. Only ticks both traces have are compared, and only
fields both traces write somewhere (each side may write more: the original's path detail and status timers, the port's
actionId).
"""
import argparse
import json
import sys
from pathlib import Path

# Fields compared when both sides write them. `action`/`cmd` are names in the original and numbers in the port, so
# they aren't here.
FIELDS = ["px", "py", "fix", "dir", "hp", "lights", "weapon", "tur", "reload", "tgt", "status"]


def load(path: Path) -> dict:
    ticks = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:  # A crashed run's last line can be cut off.
                continue
            ticks[rec["tick"]] = rec
    return ticks


def written_fields(trace: dict) -> set:
    return {k for rec in trace.values() for u in rec.get("units", []) for k in u}


def compare(a_path: Path, b_path: Path, ignore: tuple = ()) -> dict:
    a, b = load(a_path), load(b_path)
    common = sorted(set(a) & set(b))
    first: dict[str, dict] = {}
    equal_unit_ticks = unit_ticks = 0
    fields = [f for f in FIELDS if f in written_fields(a) and f in written_fields(b) and f not in ignore]
    seen_fields: set[str] = set(fields)

    def note(key: str, tick: int, va, vb):
        if key not in first:
            first[key] = {"tick": tick, "a": va, "b": vb}

    for t in common:
        ua = {u["spawn"]: u for u in a[t].get("units", []) if "spawn" in u}
        ub = {u["spawn"]: u for u in b[t].get("units", []) if "spawn" in u}
        for s in sorted(set(ua) | set(ub)):
            if s not in ua or s not in ub:
                note(f"spawn {s} present", t, s in ua, s in ub)
                continue
            unit_ticks += 1
            same = True
            for f in fields:  # A field a unit doesn't write this tick (a status effect that's off) counts as null.
                if ua[s].get(f) != ub[s].get(f):
                    same = False
                    note(f"spawn {s} {f}", t, ua[s].get(f), ub[s].get(f))
            equal_unit_ticks += same
        if "rng" in a[t] and "rng" in b[t] and "rng" not in ignore:
            seen_fields.add("rng")
            if a[t]["rng"] != b[t]["rng"]:
                note("rng", t, a[t]["rng"], b[t]["rng"])
    return {
        "a": str(a_path), "b": str(b_path), "ticks_a": len(a), "ticks_b": len(b), "ticks_compared": len(common),
        "unit_ticks": unit_ticks, "unit_ticks_equal": equal_unit_ticks, "fields": sorted(seen_fields),
        "ignored": sorted(ignore),
        "match": not first and len(common) > 0,
        "first_differences": dict(sorted(first.items(), key=lambda kv: (kv[1]["tick"], kv[0]))),
    }


def summary_line(r: dict) -> str:
    ignored = f" (ignoring {','.join(r['ignored'])})" if r.get("ignored") else ""
    if r["match"]:
        return f"MATCH: {r['ticks_compared']} ticks, {r['unit_ticks']} unit-ticks, every compared field equal{ignored}"
    d = r["first_differences"]
    t0 = min(v["tick"] for v in d.values())
    return (f"DIFF: first at tick {t0} ({len(d)} field(s) differ); {r['unit_ticks_equal']} of {r['unit_ticks']} "
            f"unit-ticks fully equal over {r['ticks_compared']} ticks{ignored}")


def print_report(r: dict, labels=("A", "B")) -> None:
    print(summary_line(r))
    for key, v in r["first_differences"].items():
        print(f"  {key}: tick {v['tick']}: {labels[0]} {json.dumps(v['a'])} / {labels[1]} {json.dumps(v['b'])}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", type=Path)
    ap.add_argument("b", type=Path)
    ap.add_argument("--labels", default="A,B", help="names for the two sides in the output")
    ap.add_argument("--json", type=Path, help="also write the report as JSON")
    ap.add_argument("--ignore", default="", metavar="FIELDS", help="fields left out, comma separated (e.g. lights,rng)")
    args = ap.parse_args()
    r = compare(args.a, args.b, tuple(f for f in args.ignore.split(",") if f))
    print_report(r, tuple(args.labels.split(",", 1)))
    if args.json:
        args.json.write_text(json.dumps(r, indent=1) + "\n")
    return 0 if r["match"] else 1


if __name__ == "__main__":
    sys.exit(main())
