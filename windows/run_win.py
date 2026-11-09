"""Runs a scenario in the original Outpost 2 natively on Windows (no Wine, no Xvfb) through harness.dll.

  python -I windows/run_win.py setup [--from "C:\\...\\Outpost 2"] [--game DIR]
      Copies the game (without movies and OPU backups) to a private folder, installs build\\win\\harness.dll and
      zScenario.dll into its OPU\\Harness and registers the module in its OPU\\outpost2.ini. Run again after a rebuild.
  python -I windows/run_win.py run SCENARIO.json OUT_DIR [--game DIR] [--shot-every N] [--no-turbo] [--timeout S]
      Writes the harness's outputs (trace.jsonl, telemetry.jsonl, gamedata.json, harness.log, shot_*.png,
      summary.json) to OUT_DIR. Compare with op2-rs: op2-inspect compare --scenario SCENARIO.json --oracle OUT_DIR

The private copy (default: game\\ next to this folder's parent) keeps harness runs from touching the player's install
and settings. The scenario translation is run.py's, imported unchanged.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = Path(r"C:\Users\Desktop\Outpost 2")
DEFAULT_GAME = ROOT / "game"

sys.modules.setdefault("fcntl", types.ModuleType("fcntl"))  # run.py imports it for --record's lock only.
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))
import run  # noqa: E402
import ini  # noqa: E402


def use_game(game: Path) -> None:
    run.GAME_DIR = game
    run.OPU_DIR = game / "OPU"


def setup(source: Path, game: Path) -> None:
    if not (source / "Outpost2.exe").exists():
        sys.exit(f"setup: no Outpost2.exe in {source}")
    if game.resolve() == source.resolve():
        sys.exit("setup: the private copy must not be the game install itself")
    print(f"setup: copying {source} -> {game}")
    subprocess.run(["robocopy", str(source), str(game), "/E", "/XF", "*.avi",
                    "/XD", str(source / "OPU" / "backups"), str(source / "OPU" / "logs"),
                    "/NFL", "/NDL", "/NJH", "/NJS", "/NP"], check=False)  # robocopy exits 1 on success.
    harness_dir = game / "OPU" / "Harness"
    harness_dir.mkdir(parents=True, exist_ok=True)
    for dll in ("harness.dll", "zScenario.dll"):
        built = ROOT / "build" / "win" / dll
        if not built.exists():
            sys.exit(f"setup: {built} missing: run windows\\build.bat first")
        shutil.copy2(built, harness_dir / dll)
    ini_path = game / "OPU" / "outpost2.ini"
    text = ini_path.read_bytes().decode("latin-1").replace("\r\n", "\n")
    new = ini.install(text)
    if new != text:
        ini_path.write_bytes(new.replace("\n", "\r\n").encode("latin-1"))
    print(f"setup: harness installed in {harness_dir}")


def to_png(out: Path) -> int:
    """BMP -> PNG through System.Drawing (no PIL needed)."""
    bmps = sorted(out.glob("shot_*.bmp"))
    if bmps:
        ps = "Add-Type -AssemblyName System.Drawing; " + "; ".join(
            f"$i=[System.Drawing.Image]::FromFile('{b}'); $i.Save('{b.with_suffix('.png')}', "
            f"[System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); Remove-Item '{b}'" for b in bmps)
        subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=False)
    return len(bmps)


def run_scenario(a) -> int:
    if not (run.OPU_DIR / "Harness" / "harness.dll").exists():
        sys.exit(f"run: no harness in {run.GAME_DIR}: run `run_win.py setup` first")
    scn = json.loads(a.scenario.read_text())
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for old in [*out.glob("shot_*"), *(out / n for n in ("trace.jsonl", "telemetry.jsonl", "harness.log",
                                                        "gamedata.json", "summary.json"))]:
        old.unlink(missing_ok=True)
    ticks, seed = scn.get("ticks", 3000), scn.get("seed", 1)
    mission = run.scenario_mission(scn.get("map", "cm01.map"), scn.get("techtree", "multitek.txt"))
    script = out / "scenario_script.txt"
    script.write_text(run.scenario_script(scn))
    (out / "scenario.json").write_text(json.dumps(dict(scn, seed=seed, ticks=ticks), indent=1) + "\n")
    env = dict(os.environ, OP2H_MISSION=mission, OP2H_OUT=str(out), OP2H_SCRIPT=str(script),
               OP2H_TICKS=str(ticks), OP2H_SEED=str(seed), OP2H_DIFFICULTY=str(scn.get("difficulty", 1)),
               OP2H_TRACE_EVERY=str(scn.get("traceEvery", 1)), OP2H_TELEM_EVERY=str(scn.get("telemetryEvery", 100)),
               OP2H_TRACE_MAP="1" if scn.get("traceMap") else "0", OP2H_ECO="1" if scn.get("eco") else "0",
               OP2H_AUDIO="1" if scn.get("audio") else "0", OP2H_SHOT_EVERY=str(a.shot_every),
               OP2H_TURBO="0" if a.no_turbo else "1", OP2H_SPEED="10", OP2H_WIDTH="1024", OP2H_HEIGHT="768")
    start = time.monotonic()
    game = subprocess.Popen([str(run.GAME_DIR / "Outpost2.exe"), "/OPU"], cwd=run.GAME_DIR, env=env)
    try:
        code, timed_out = game.wait(timeout=a.timeout), False
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(game.pid)], capture_output=True)
        code, timed_out = game.wait(), True
    hlog = (out / "harness.log").read_text(errors="replace") if (out / "harness.log").exists() else ""
    quit_line = next((l for l in hlog.splitlines() if l.startswith("quit at tick")), None)
    summary = {"scenario": str(a.scenario), "mission": mission, "exit_code": code, "timed_out": timed_out,
               "wall_seconds": round(time.monotonic() - start, 1), "screenshots": to_png(out),
               "dialogs": [l[len("dialog: "):] for l in hlog.splitlines() if l.startswith("dialog: ")],
               "quit": quit_line, "ok": bool(quit_line) and not timed_out and code == 0}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))
    return 0 if summary["ok"] else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup", help="make the private game copy and install the harness into it")
    s.add_argument("--from", dest="source", type=Path, default=DEFAULT_SOURCE, help="the game install to copy")
    s.add_argument("--game", type=Path, default=DEFAULT_GAME, help="the private copy")
    r = sub.add_parser("run", help="run a scenario in the original game")
    r.add_argument("scenario", type=Path)
    r.add_argument("out", type=Path)
    r.add_argument("--game", type=Path, default=DEFAULT_GAME, help="the private copy")
    r.add_argument("--shot-every", type=int, default=0, metavar="N", help="screenshot every N ticks (0 = off)")
    r.add_argument("--no-turbo", action="store_true", help="real time at full game speed, to watch it")
    r.add_argument("--timeout", type=float, default=600, help="wall-clock seconds before the game is killed")
    a = ap.parse_args()
    use_game(a.game.resolve())
    if a.cmd == "setup":
        setup(a.source.resolve(), a.game.resolve())
        return 0
    return run_scenario(a)


if __name__ == "__main__":
    sys.exit(main())
