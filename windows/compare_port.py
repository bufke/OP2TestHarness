"""The original Outpost 2 vs outpost2-port (and optionally op2-rs) on one or more scenarios, on Windows.

  python -I windows/compare_port.py SCENARIO.json [SCENARIO.json ...] [--tick T] [--out DIR] [--op2rs [DIR]]
                                    [--no-pictures] [--game DIR]

For each scenario:
1. the original plays it (run_win.py run), with a screenshot at tick T;
2. outpost2-port plays it (run_port.py run, its --harness mode), with a screenshot at tick T;
3. trace_diff.py compares the two traces: MATCH, or the first tick each field differs (port_diff.txt, port_diff.json);
4. with --op2rs, op2-sim is compared with the original too (op2-inspect compare) and op2-engine draws tick T;
5. side_by_side.png: original | port (| op2-rs) at tick T.
With several scenarios it ends with one line per scenario. Outputs go to build\\compare-port\\<scenario>\\ (--out
for one scenario). Needs run_win.py setup and run_port.py install done; --op2rs needs op2-rs's tools built.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare  # noqa: E402
import run_port  # noqa: E402
import run_win  # noqa: E402
import trace_diff  # noqa: E402

ROOT = run_win.ROOT


def one(scn_path: Path, a, out: Path) -> dict:
    scn = json.loads(scn_path.read_text())
    tick = a.tick or max(5, scn.get("ticks", 3000) // 2)
    shot_every = 0 if a.no_pictures else tick
    print(f"== {scn_path.stem}: {scn.get('ticks', 3000)} ticks" + ("" if a.no_pictures else f", pictures at tick {tick}"))

    original = out / "original"
    run_win.use_game(a.game.resolve())
    if run_win.run_scenario(argparse.Namespace(scenario=scn_path, out=original, shot_every=shot_every, no_turbo=False,
                                               timeout=600)):
        return {"scenario": scn_path.stem, "line": "the original's run failed (original\\harness.log)"}
    port = out / "port"
    if run_port.run_scenario(argparse.Namespace(scenario=scn_path, out=port, game=a.game, shot_every=shot_every,
                                                timeout=600, show=False)):
        return {"scenario": scn_path.stem, "line": "the port's run failed (port\\harness.log, port_stdout.txt)"}

    report = trace_diff.compare(original / "trace.jsonl", port / "trace.jsonl", a.ignore)
    (out / "port_diff.json").write_text(json.dumps(report, indent=1) + "\n")
    lines = [trace_diff.summary_line(report)] + [
        f"  {k}: tick {v['tick']}: original {json.dumps(v['a'])} / port {json.dumps(v['b'])}"
        for k, v in report["first_differences"].items()]
    (out / "port_diff.txt").write_text("\n".join(lines) + "\n")
    print("port vs original: " + "\n".join(lines))
    result = {"scenario": scn_path.stem, "line": "port " + trace_diff.summary_line(report), "match": report["match"]}

    panels = []
    if not a.no_pictures:
        panels = [(original / f"shot_{tick:07d}.png", f"Outpost 2 (original), tick {tick}"),
                  (port / f"shot_{tick:07d}.png", f"outpost2-port, tick {tick}")]
    if a.op2rs:
        inspect, engine = compare.op2rs_tools(a.op2rs)
        sim = compare.op2rs_sim(inspect, scn_path, original, out, a.source)
        verdict = next((l for l in sim.splitlines() if l.startswith(("MATCH", "DIFF", "DIVERGE"))), sim.splitlines()[-1] if sim else "?")
        print(f"op2-rs vs original: {verdict.split(':')[0]}")
        result["line"] += f"; op2-rs {verdict.split(':')[0]}"
        if panels:
            panels.append((compare.op2rs_picture(engine, scn_path, scn, tick, original, out, a.source), f"op2-rs, tick {tick}"))
    missing = [str(p) for p, _ in panels if not p.exists()]
    if panels and not missing:
        compare.side_by_side(panels, out / "side_by_side.png")
        print(f"side by side: {out / 'side_by_side.png'}")
    elif missing:
        print(f"no side by side: missing {', '.join(missing)}")
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenarios", type=Path, nargs="+")
    ap.add_argument("--tick", type=int, help="tick to picture (default: half each scenario's length)")
    ap.add_argument("--out", type=Path, help="output folder (one scenario only; default build\\compare-port\\<name>)")
    ap.add_argument("--op2rs", type=Path, nargs="?", const=compare.DEFAULT_OP2RS,
                    help="also compare op2-rs (op2-sim and op2-engine) from this checkout (default: the usual one)")
    ap.add_argument("--no-pictures", action="store_true", help="traces only: no screenshots or side by side")
    ap.add_argument("--ignore", default="", metavar="FIELDS",
                    help="trace fields left out of the port's comparison, comma separated (e.g. lights,rng)")
    ap.add_argument("--game", type=Path, default=run_win.DEFAULT_GAME, help="the private game copy")
    ap.add_argument("--source", type=Path, default=run_win.DEFAULT_SOURCE, help="the game install op2-rs reads data from")
    a = ap.parse_args()
    a.ignore = tuple(f for f in a.ignore.split(",") if f)
    if a.out and len(a.scenarios) > 1:
        ap.error("--out takes one scenario")
    results = []
    for s in a.scenarios:
        s = s.resolve()
        out = (a.out or ROOT / "build" / "compare-port" / s.stem).resolve()
        out.mkdir(parents=True, exist_ok=True)
        results.append(one(s, a, out))
    if len(results) > 1:
        print("\n== summary")
        width = max(len(r["scenario"]) for r in results)
        for r in results:
            print(f"{r['scenario']:<{width}}  {r['line']}")
        print(f"{sum(1 for r in results if r.get('match'))} of {len(results)} match the original")
    return 0 if all(r.get("match") for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
