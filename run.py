#!/usr/bin/env python3
"""Run an Outpost 2 mission headless under Xvfb via the Harness op2ext module and collect its output.

Example:
    harness/run.sh --mission cHello.dll --ticks 3000 --screenshot-every 500 --out /tmp/op2run

    harness/run.sh --scenario harness/oracle/scenarios/lynx_fast.json --out /tmp/op2scn

Outputs in --out: telemetry.jsonl, trace.jsonl, gamedata.json, shot_<tick>.png, harness.log, wine.log, summary.json.
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Harness runs use their own Wine prefix when one exists (prefix-harness/, a copy of prefix/), so they get their own
# wineserver and can't steal focus from, or swap DLLs under, a game David is playing from prefix/.
PLAY_PREFIX = ROOT / "prefix"
PREFIX = Path(os.environ.get("OP2_HARNESS_PREFIX") or
              (ROOT / "prefix-harness" if (ROOT / "prefix-harness").is_dir() else PLAY_PREFIX))
GAME_DIR = PREFIX / "drive_c" / "GOG Games" / "Outpost 2"
OPU_DIR = GAME_DIR / "OPU"


def sync_missions_from_play_prefix():
    """Mission DLLs get installed into prefix/ (missions/Makefile); mirror OPU/maps into the harness prefix."""
    if PREFIX.resolve() == PLAY_PREFIX.resolve():
        return
    src = PLAY_PREFIX / "drive_c" / "GOG Games" / "Outpost 2" / "OPU" / "maps"
    if src.is_dir():
        shutil.copytree(src, OPU_DIR / "maps", dirs_exist_ok=True)


# GameTermReasons from TethysAPI Game/GameStartInfo.h.
END_REASONS = {0: "Running", 3: "MissionAccomplished", 4: "MissionFailed", 6: "MissionAborted", 7: "Victory",
               8: "Defeat", 9: "ControlTerminated", 12: "GameOver"}


# CellType names from TethysAPI Game/MapImpl.h, for scenario "cells" overrides given by name.
CELL_TYPES = ["FastPassible1", "Impassible2", "SlowPassible1", "SlowPassible2", "MediumPassible1", "MediumPassible2",
              "Impassible1", "FastPassible2", "NorthCliffs", "CliffsHighSide", "CliffsLowSide", "VentsAndFumaroles",
              *[f"ZPad{i}" for i in range(12, 21)], "DozedArea", "Rubble", "NormalWall", "MicrobeWall", "LavaWall",
              *[f"Tube{i}" for i in range(6)]]

# The scenario mission ships next to harness.dll rather than in OPU/maps, so it never shows up in the game's lists.
# The game reads a mission's MapName export from the DLL file itself, so each map gets a patched copy there.
MAPNAME_MARKER = b"@@OP2H_MAPNAME_SLOT@@"


def scenario_mission(map_name: str) -> str:
    """Returns the mission name (relative to the game folder) of zScenario.dll patched to load `map_name`."""
    src = OPU_DIR / "Harness" / "zScenario.dll"
    if not src.exists():
        sys.exit("scenario mission not installed: run `make -C harness install`")
    data = bytearray(src.read_bytes())
    at = data.find(MAPNAME_MARKER) - len(b"cm01.map\0")
    if at < 0 or data[at:at + 9] != b"cm01.map\0":
        sys.exit("zScenario.dll has no MapName slot; rebuild with `make -C harness install`")
    name = map_name.encode("ascii")
    if len(name) > 63:
        sys.exit(f"map name too long for the mission's MapName: {map_name}")
    data[at:at + 64] = name + bytes(64 - len(name))
    stem = Path(map_name).stem.lower()
    # GameStartInfo::scriptName holds 31 chars + NUL: "OPU\\Harness\\" + "zs_" + up to 12 + ".dll".
    if len(stem) > 12 or not re.fullmatch(r"[a-z0-9_-]+", stem):
        stem = format(zlib.crc32(map_name.encode()), "08x")
    dll = f"zs_{stem}.dll"
    dst = OPU_DIR / "Harness" / dll
    if not dst.exists() or dst.read_bytes() != data:
        tmp = dst.with_suffix(f".{os.getpid()}.tmp")
        tmp.write_bytes(data)
        os.replace(tmp, dst)  # Atomic, so parallel runs on the same map never see a partial file.
    return f"OPU\\Harness\\{dll}"


def scenario_script(scn: dict) -> str:
    """Translates a scenario JSON (ENGINE_PLAN.md format) into a harness script in file tile coordinates."""
    lines = ["# generated from a scenario by run.py", "0 coords file"]
    for c in scn.get("setcell", []) + scn.get("cells", []):
        cell = c["cell"] if isinstance(c["cell"], int) else CELL_TYPES.index(c["cell"])
        x1, y1, x2, y2 = c["rect"]
        lines.append(f"0 setcell {x1} {y1} {x2} {y2} {cell}")
    for s in scn.get("spawn", []):
        x, y = s["tile"]
        lines.append(f"0 spawn {s['type']} {s.get('owner', 0)} {x} {y} {s.get('weapon') or '-'} {s.get('dir', 0)}")
    # Centre the view (for screenshots) on the scenario's "view" tile, else on the spawns.
    spawns = scn.get("spawn", [])
    view = scn.get("view") or (spawns and [sum(s["tile"][i] for s in spawns) // len(spawns) for i in (0, 1)])
    if view:
        lines.append(f"0 view {view[0]} {view[1]}")
    for o in scn.get("orders", []):
        cmd, unit, tick = o["cmd"].lower(), f"@{o['unit']}", o["tick"]
        if cmd == "move":
            lines.append(f"{tick} move {unit} {o['tile'][0]} {o['tile'][1]}")
        elif cmd == "attack":
            lines.append(f"{tick} attack {unit} @{o['target']}")
        elif cmd in ("stop", "selfdestruct"):
            lines.append(f"{tick} {cmd} {unit}")
        else:
            sys.exit(f"scenario: unsupported order {o}")
    # Raw script lines (e.g. input verbs: "20 click tile 40 30"), still in file coordinates.
    lines += scn.get("script", [])
    return "\n".join(lines) + "\n"


def win_path(p: Path) -> str:
    return "Z:" + str(p).replace("/", "\\")


def check_installed() -> None:
    ini = (OPU_DIR / "outpost2.ini").read_text(errors="replace")
    if not (OPU_DIR / "Harness" / "harness.dll").exists() or "Harness=yes" not in ini:
        sys.exit("harness module not installed: run `make -C harness install`")


def start_xvfb(width: int, height: int) -> tuple[subprocess.Popen, str]:
    r, w = os.pipe()
    proc = subprocess.Popen(
        ["Xvfb", "-displayfd", str(w), "-screen", "0", f"{width + 256}x{height + 256}x24", "-nolisten", "tcp"],
        pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(w)
    with os.fdopen(r) as f:
        num = f.readline().strip()
    if not num:
        proc.kill()
        sys.exit("Xvfb failed to start")
    return proc, ":" + num


def grab_screen(display: str, path: Path) -> None:
    try:
        from PIL import ImageGrab
        ImageGrab.grab(xdisplay=display).save(path)
    except Exception as e:  # Diagnostics only.
        print(f"screen grab failed: {e}", file=sys.stderr)


def convert_shots(out: Path, keep_bmp: bool) -> int:
    try:
        from PIL import Image
    except ImportError:
        print("PIL not available; leaving BMP screenshots", file=sys.stderr)
        return len(list(out.glob("shot_*.bmp")))
    n = 0
    for bmp in sorted(out.glob("shot_*.bmp")):
        Image.open(bmp).save(bmp.with_suffix(".png"), optimize=False)
        if not keep_bmp:
            bmp.unlink()
        n += 1
    return n


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mission", help="mission DLL file name, e.g. cHello.dll (searched like the game does)")
    ap.add_argument("--scenario", type=Path,
                    help="scenario JSON (map, seed, ticks, spawn, orders); runs the built-in scenario mission")
    ap.add_argument("--out", required=True, type=Path, help="output directory (created; existing outputs overwritten)")
    ap.add_argument("--ticks", type=int, help="stop after this game tick (100 ticks = 1 mark); 0 = never. "
                    "Default 3000, or the scenario's ticks")
    ap.add_argument("--screenshot-every", type=int, default=0, metavar="N", help="screenshot every N ticks (0 = off)")
    ap.add_argument("--telemetry-every", type=int, default=100, metavar="N", help="telemetry every N ticks (0 = off)")
    ap.add_argument("--trace-every", type=int, metavar="N",
                    help="trace.jsonl (sub-tile positions) every N ticks; default 1 with --scenario, else 0 (off)")
    ap.add_argument("--trace-raw", action="store_true", help="add raw MapObject/PathContext bytes to trace lines")
    ap.add_argument("--speed", type=int, default=10, help="in-game speed setting 1-10 (only matters with --no-turbo)")
    ap.add_argument("--no-turbo", action="store_true", help="keep the game's frame limiter (real-time at --speed)")
    ap.add_argument("--draw-every", type=int, default=0, metavar="N",
                    help="turbo: render every N ticks (default: only right before screenshots; 1 = always)")
    ap.add_argument("--seed", type=int, help="random number seed (default 1, or the scenario's seed)")
    ap.add_argument("--difficulty", type=int, default=1, choices=(0, 1, 2), help="0 easy, 1 normal, 2 hard")
    ap.add_argument("--script", type=Path,
                    help="scripted command file (see README); with --scenario, appended to the scenario's script")
    ap.add_argument("--bot", type=Path, help="bot DLL exporting HarnessBotTick(int tick)")
    ap.add_argument("--size", default="1024x768", help="game client area WxH")
    ap.add_argument("--timeout", type=float, default=600, help="wall-clock seconds before the game is killed")
    ap.add_argument("--keep-bmp", action="store_true", help="keep raw BMP screenshots next to the PNGs")
    ap.add_argument("--display", help="use this X display instead of starting a private Xvfb (e.g. to watch)")
    args = ap.parse_args()
    scn = json.loads(args.scenario.read_text()) if args.scenario else None
    if bool(args.mission) == bool(scn):
        ap.error("give exactly one of --mission and --scenario")
    if args.ticks is None:
        args.ticks = scn.get("ticks", 3000) if scn else 3000
    if args.seed is None:
        args.seed = scn.get("seed", 1) if scn else 1
    if args.trace_every is None:
        args.trace_every = 1 if scn else 0

    sync_missions_from_play_prefix()
    check_installed()
    width, height = (int(v) for v in args.size.lower().split("x"))
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for old in [*out.glob("shot_*.bmp"), *out.glob("shot_*.png"), out / "telemetry.jsonl", out / "harness.log",
                out / "timeout_screen.png", out / "summary.json", out / "trace.jsonl", out / "gamedata.json",
                out / "scenario_script.txt"]:
        old.unlink(missing_ok=True)
    mission = args.mission
    if scn:
        mission = scenario_mission(scn.get("map", "cm01.map"))
        extra = args.script.read_text() if args.script else ""  # --script lines run after the scenario's own.
        args.script = out / "scenario_script.txt"
        args.script.write_text(scenario_script(scn) + extra)
        (out / "scenario.json").write_text(json.dumps(scn, indent=1) + "\n")

    xvfb, display = (None, args.display) if args.display else start_xvfb(width, height)
    env = dict(os.environ,
               DISPLAY=display,
               WINEPREFIX=str(PREFIX),
               WINEDEBUG="-all",
               # No audio device (silent, leaves the user's sound server alone); no winedbg crash dialog (a crash
               # ends the run instead of hanging it until --timeout).
               WINEDLLOVERRIDES="winepulse.drv=d;winealsa.drv=d;winedbg.exe=d",
               OP2H_MISSION=mission,
               OP2H_TRACE_EVERY=str(args.trace_every),
               OP2H_TRACE_RAW="1" if args.trace_raw else "0",
               OP2H_OUT=win_path(out),
               OP2H_TICKS=str(args.ticks),
               OP2H_SHOT_EVERY=str(args.screenshot_every),
               OP2H_TELEM_EVERY=str(args.telemetry_every),
               OP2H_SPEED=str(args.speed),
               OP2H_TURBO="0" if args.no_turbo else "1",
               OP2H_DRAW_EVERY=str(args.draw_every),
               OP2H_SEED=str(args.seed),
               OP2H_DIFFICULTY=str(args.difficulty),
               OP2H_WIDTH=str(width),
               OP2H_HEIGHT=str(height))
    if args.script:
        env["OP2H_SCRIPT"] = win_path(args.script.resolve())
    if args.bot:
        env["OP2H_BOT"] = win_path(args.bot.resolve())
    env.pop("WAYLAND_DISPLAY", None)  # Make sure Wine uses the X display we picked.

    start = time.monotonic()
    timed_out = False
    with open(out / "wine.log", "w") as log:
        game = subprocess.Popen(["wine", "Outpost2.exe", "/OPU"], cwd=GAME_DIR, env=env,
                                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = game.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            grab_screen(display, out / "timeout_screen.png")  # Usually shows the modal dialog that hung the game.
            os.killpg(game.pid, signal.SIGKILL)
            code = game.wait()
    elapsed = time.monotonic() - start
    if xvfb:
        xvfb.terminate()
        xvfb.wait()

    shots = convert_shots(out, args.keep_bmp)
    hlog = (out / "harness.log").read_text(errors="replace") if (out / "harness.log").exists() else ""
    last_tick, end = 0, None
    telem = out / "telemetry.jsonl"
    if telem.exists():
        for line in reversed(telem.read_text(errors="replace").splitlines()):
            try:
                last = json.loads(line)  # The final line can be truncated if the game crashed mid-write.
            except json.JSONDecodeError:
                continue
            last_tick, end = last["tick"], last["end"]
            break
    quit_line = next((l for l in hlog.splitlines() if l.startswith("quit at tick")), None)
    summary = {
        "mission": mission, "scenario": str(args.scenario) if scn else None, "exit_code": code, "timed_out": timed_out, "wall_seconds": round(elapsed, 1),
        "last_telemetry_tick": last_tick, "game_term_reason": END_REASONS.get(end, end), "screenshots": shots,
        "dialogs": [l[len("dialog: "):] for l in hlog.splitlines() if l.startswith("dialog: ")],
        "quit": quit_line, "ok": bool(quit_line) and not timed_out and code == 0,
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
