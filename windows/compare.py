"""The original Outpost 2 vs op2-rs on one scenario, on Windows: simulation compared tick by tick, pictures side by side.

  python -I windows/compare.py SCENARIO.json [--tick T] [--out DIR] [--op2rs C:\\...\\op2-rs] [--game DIR]

1. Plays the scenario in the original game (run_win.py run), with a screenshot at tick T.
2. `op2-inspect compare`: op2-sim replays the same scenario against the original's trace (MATCH or the first
   divergence of every field) and plots both trajectories.
3. op2-rs's engine (op2-engine) plays the scenario to tick T and saves its window.
4. original_T.png | op2-rs_T.png side by side in side_by_side.png.
Needs run_win.py setup done, and op2-rs's op2-inspect and op2-engine built (cargo build -p op2-inspect -p op2-engine).
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_win  # noqa: E402

DEFAULT_OP2RS = Path(r"C:\Users\Desktop\GITHUB\op2-rs")


PANEL_COLOURS = ["Orange", "Cyan", "LightGreen", "Violet"]


def side_by_side(panels: list, out: Path) -> None:
    """Pictures in a row with a label above each: panels is [(png path, label), ...]."""
    loads = "; ".join(f"$i{k}=[System.Drawing.Image]::FromFile('{p}')" for k, (p, _) in enumerate(panels))
    imgs = ",".join(f"$i{k}" for k in range(len(panels)))
    labels = ",".join(f"'{label}'" for _, label in panels)
    colours = ",".join(f"'{PANEL_COLOURS[k % len(PANEL_COLOURS)]}'" for k in range(len(panels)))
    ps = f"""
Add-Type -AssemblyName System.Drawing
{loads}; $imgs=@({imgs}); $labels=@({labels}); $colours=@({colours})
[int]$w=8*($imgs.Count-1); [int]$h=0; foreach ($i in $imgs) {{ $w+=$i.Width; if ($i.Height -gt $h) {{ $h=$i.Height }} }}; $h+=28
$c=New-Object System.Drawing.Bitmap $w,$h; $g=[System.Drawing.Graphics]::FromImage($c); $g.Clear([System.Drawing.Color]::Black)
$f=New-Object System.Drawing.Font 'Segoe UI',12,([System.Drawing.FontStyle]::Bold)
$x=0; for ($k=0; $k -lt $imgs.Count; $k++) {{
  $b=New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromName($colours[$k]))
  $g.DrawString($labels[$k],$f,$b,$x+4,4); $g.DrawImage($imgs[$k],$x,28,$imgs[$k].Width,$imgs[$k].Height); $x+=$imgs[$k].Width+8 }}
$c.Save('{out}',[System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $c.Dispose(); foreach ($i in $imgs) {{ $i.Dispose() }}
"""
    out.unlink(missing_ok=True)
    subprocess.run(["powershell", "-NoProfile", "-Command", "$ErrorActionPreference='Stop'; " + ps], check=True)
    if not out.exists():
        sys.exit(f"compare: no side by side written to {out}")


def op2rs_tools(op2rs: Path) -> tuple:
    tools = op2rs / "tools" / "target" / "debug"
    inspect, engine = tools / "op2-inspect.exe", tools / "op2-engine.exe"
    for exe in (inspect, engine):
        if not exe.exists():
            sys.exit(f"compare: {exe} missing: cargo build -p op2-inspect -p op2-engine in {op2rs / 'tools'}")
    return inspect, engine


def op2rs_sim(inspect: Path, scn_path: Path, original: Path, out: Path, source: Path) -> str:
    """op2-sim against the original's trace (op2-inspect compare); writes sim_compare.txt, returns the report."""
    sim = subprocess.run([str(inspect), "compare", "--scenario", str(scn_path), "--oracle", str(original),
                          "--out", str(out / "sim")], env=dict(os.environ, OP2_DIR=str(source)),
                         capture_output=True, text=True)
    report = (sim.stdout + sim.stderr).strip()
    (out / "sim_compare.txt").write_text(newline="\n", data=report + "\n")
    return report


def op2rs_picture(engine: Path, scn_path: Path, scn: dict, tick: int, original: Path, out: Path, source: Path) -> Path:
    """op2-engine's picture of the scenario at `tick` (1024x768, 1x), centred as the harness centres it."""
    spawns = scn.get("spawn", [])
    view = scn.get("view") or (spawns and [sum(s["tile"][i] for s in spawns) // len(spawns) for i in (0, 1)])
    shot = out / f"op2-rs_{tick:07d}.png"
    cmd = [str(engine), "--scenario", str(scn_path), "--until-tick", str(tick), "--screenshot", str(shot),
           "--only-draw-shot", "--mute", "--native-decorations", "--scale", "1", "--size", "1024x768",
           "--rules", str(original / "gamedata.json"), "--game-dir", str(source), "--no-record"]
    if view:
        cmd += ["--focus", f"{view[0]},{view[1]}"]
    eng = subprocess.run(cmd, env=dict(os.environ, OP2_DIR=str(source)), capture_output=True, text=True, timeout=600)
    (out / "engine.log").write_text(newline="\n", data=eng.stdout + eng.stderr)
    if not shot.exists():
        sys.exit(f"compare: op2-engine saved no picture (exit {eng.returncode}; see engine.log)")
    return shot


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", type=Path)
    ap.add_argument("--tick", type=int, help="tick to picture (default: half the scenario's length)")
    ap.add_argument("--out", type=Path, help="output folder (default build\\compare\\<scenario name>)")
    ap.add_argument("--op2rs", type=Path, default=DEFAULT_OP2RS, help="op2-rs checkout with built tools")
    ap.add_argument("--game", type=Path, default=run_win.DEFAULT_GAME, help="the private game copy (run_win.py setup)")
    ap.add_argument("--source", type=Path, default=run_win.DEFAULT_SOURCE, help="the game install op2-rs reads data from")
    a = ap.parse_args()
    scn_path = a.scenario.resolve()
    scn = json.loads(scn_path.read_text())
    tick = a.tick or max(5, scn.get("ticks", 3000) // 2)
    out = (a.out or ROOT / "build" / "compare" / scn_path.stem).resolve()
    inspect, engine = op2rs_tools(a.op2rs)

    print(f"== 1. original game: {scn_path.name}, picture at tick {tick}")
    original = out / "original"
    run_win.use_game(a.game.resolve())
    rc = run_win.run_scenario(argparse.Namespace(scenario=scn_path, out=original, shot_every=tick, no_turbo=False,
                                                 timeout=600))
    if rc:
        sys.exit("compare: the original's run failed (see original\\summary.json and harness.log)")

    print("== 2. op2-sim vs the original, tick by tick")
    report = op2rs_sim(inspect, scn_path, original, out, a.source)
    print("\n".join(l for l in report.splitlines() if not l.startswith(("rules:", "report:", "image:"))))

    print(f"== 3. op2-rs engine at tick {tick}")
    shot = op2rs_picture(engine, scn_path, scn, tick, original, out, a.source)

    orig_shot = original / f"shot_{tick:07d}.png"
    sbs = out / "side_by_side.png"
    side_by_side([(orig_shot, f"Outpost 2 (original), tick {tick}"), (shot, f"op2-rs, tick {tick}")], sbs)
    print(f"== done\n  side by side: {sbs}\n  trajectories: {out / 'sim' / 'plot.png'}\n  report: {out / 'sim_compare.txt'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
