"""Runs a scenario in outpost2-port (github.com/leviathan400/outpost2-port) through its --harness mode, from the private
game copy, writing the same files as run_win.py does for the original.

  python -I windows/run_port.py install [--port C:\\...\\outpost2-port] [--game DIR]
      Copies the port's built outpost2_modern.exe (build\\Release) and SDL2.dll into the game copy. Run it after each
      build of the port.
  python -I windows/run_port.py run SCENARIO.json OUT_DIR [--game DIR] [--shot-every N] [--timeout S] [--show]
      Writes trace.jsonl, harness.log, shot_*.png, port_stdout.txt and summary.json to OUT_DIR. --show keeps the
      port's window visible.

The script is run.py's translation of the scenario, the one the original plays (scenario_script.txt).
"""
import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_win  # noqa: E402  (also imports run.py, the scenario translator)

ROOT = run_win.ROOT
DEFAULT_PORT = Path(r"C:\Users\Desktop\GITHUB\outpost2-port")
PORT_EXE = "outpost2_modern.exe"


def install(port: Path, game: Path) -> None:
    exe = port / "build" / "Release" / PORT_EXE
    sdl = port / "SDL2.dll"
    for f in (exe, sdl):
        if not f.exists():
            sys.exit(f"install: {f} missing: build the port first (cmake --build build --config Release)")
    if not (game / "Outpost2.exe").exists():
        sys.exit(f"install: no game copy at {game}: run `run_win.py setup` first")
    for f in (exe, sdl):
        shutil.copy2(f, game / f.name)
    print(f"install: {PORT_EXE} and SDL2.dll from {port} -> {game}")


def run_scenario(a) -> int:
    game = a.game.resolve()
    exe = game / PORT_EXE
    if not exe.exists():
        sys.exit(f"run: no {PORT_EXE} in {game}: run `run_port.py install` first")
    scn_path = a.scenario.resolve()
    scn = json.loads(scn_path.read_text())
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for old in [*out.glob("shot_*"), *(out / n for n in ("trace.jsonl", "harness.log", "summary.json", "port_stdout.txt"))]:
        old.unlink(missing_ok=True)
    ticks, seed = scn.get("ticks", 3000), scn.get("seed", 1)
    script = out / "scenario_script.txt"
    script.write_text(newline="\n", data=run_win.run.scenario_script(scn))
    (out / "scenario.json").write_text(newline="\n",
                                       data=json.dumps(dict(scn, seed=seed, ticks=ticks), indent=1) + "\n")
    cmd = [str(exe), "--harness", "--script", str(script), "--out", str(out), "--map", scn.get("map", "cm01.map"),
           "--techtree", scn.get("techtree", "multitek.txt"), "--ticks", str(ticks), "--seed", str(seed),
           "--trace-every", str(scn.get("traceEvery", 1)), "--shot-every", str(a.shot_every)]
    if a.show:
        cmd.append("--show")
    start = time.monotonic()
    with open(out / "port_stdout.txt", "w") as log:
        game_proc = subprocess.Popen(cmd, cwd=game, stdout=log, stderr=subprocess.STDOUT)
        try:
            code, timed_out = game_proc.wait(timeout=a.timeout), False
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(game_proc.pid)], capture_output=True)
            code, timed_out = game_proc.wait(), True
    hlog = (out / "harness.log").read_text(errors="replace") if (out / "harness.log").exists() else ""
    quit_line = next((l for l in hlog.splitlines() if l.startswith("quit at tick")), None)
    unsupported = [l for l in hlog.splitlines() if "not supported here" in l]
    summary = {"scenario": str(a.scenario), "game": "outpost2-port", "exit_code": code, "timed_out": timed_out,
               "wall_seconds": round(time.monotonic() - start, 1), "screenshots": run_win.to_png(out),
               "unsupported": unsupported, "quit": quit_line, "ok": bool(quit_line) and not timed_out and code == 0}
    (out / "summary.json").write_text(newline="\n", data=json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))
    return 0 if summary["ok"] else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("install", help="copy the port's build into the game copy")
    i.add_argument("--port", type=Path, default=DEFAULT_PORT, help="outpost2-port checkout (built)")
    i.add_argument("--game", type=Path, default=run_win.DEFAULT_GAME, help="the private game copy")
    r = sub.add_parser("run", help="run a scenario in the port")
    r.add_argument("scenario", type=Path)
    r.add_argument("out", type=Path)
    r.add_argument("--game", type=Path, default=run_win.DEFAULT_GAME, help="the private game copy")
    r.add_argument("--shot-every", type=int, default=0, metavar="N", help="screenshot every N ticks (0 = off)")
    r.add_argument("--timeout", type=float, default=600, help="wall-clock seconds before the port is killed")
    r.add_argument("--show", action="store_true", help="keep the port's window visible")
    a = ap.parse_args()
    if a.cmd == "install":
        install(a.port.resolve(), a.game.resolve())
        return 0
    return run_scenario(a)


if __name__ == "__main__":
    sys.exit(main())
