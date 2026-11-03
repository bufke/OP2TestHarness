#!/usr/bin/env python3
"""Play a two-player LAN game of the original between two copies of the game under Wine on this machine, and log it
from inside both (docs/multiplayer.md "Stage 2a").

    OP2_HARNESS_PREFIX=$PWD/prefix-net harness/netgame.py --out /tmp/net --ticks 3000 \\
        --script0 harness/oracle/net/ml2_08_p0.txt --script1 harness/oracle/net/ml2_08_p1.txt --turbo

Two shell-mode runs (shell.py --net) boot the game normally in their own Xvfb and Wine virtual desktop, in the same
prefix and wineserver: the host clicks MULTIPLAYER > INTERNET (TCP/IP) > CREATE > OK (the lobby's default map, 2P Last
One Standing "Ambush Alley", ml2_08.dll; Plymouth, high resources), the joiner MULTIPLAYER > INTERNET > JOIN > OK, types
127.0.0.1, picks the session and ticks Ready, and the host presses START. The two drivers wait for each other through
files in OUT/sync (shell.py `touch` and `waitfile`). Each game's tick script gives its own player's orders, which go
through the original's network turns (harness.cpp "Network games"). Outputs: OUT/host and OUT/join, each with
start_info.json, packets.jsonl, net.jsonl, telemetry.jsonl and the shell run's files; OUT/summary.json says whether the
two games' telemetry agree, line for line, and where they first differ.

Use a prefix nobody else is running (prefix-net, a copy of prefix-harness: tools that run harness/run.sh share
prefix-harness, and its wineserver would host both games). It refuses prefix/ and prefix-harness/.
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent

HOST = """wait dialog text=~MAIN timeout=60
sleep 1.5
ctl MULTIPLAYER
sleep 2
ctl INTERNET
sleep 2
ctl CREATE
sleep 3
ctl id=1
sleep 4
dump lobby
touch host_lobby
waitfile joiner_ready timeout=120
sleep 3
shot lobby_ready
ctl id=1045
wait game what=first_cycle timeout=60
touch host_game
wait game what=tick tick={ticks} timeout={game_timeout}
shot game_end
touch host_done
waitfile join_done timeout=30
quit
"""

JOIN = """waitfile host_lobby timeout=120
wait dialog text=~MAIN timeout=60
sleep 1.5
ctl MULTIPLAYER
sleep 2
ctl INTERNET
sleep 2
ctl JOIN
sleep 3
ctl id=1 in=JOIN
sleep 3
ctl id=1001
sleep 0.5
key 1 2 7 period 0 period 0 period 1
sleep 0.5
ctl id=1 in=FIND
sleep 6
# The session list's first row (the list box's top + 26 px).
click 300 249
sleep 0.5
ctl id=1 in=PICK
sleep 6
ctl id=40007
sleep 1
shot lobby_joined
touch joiner_ready
wait game what=first_cycle timeout=90
wait game what=tick tick={ticks} timeout={game_timeout}
shot game_end
touch join_done
waitfile host_done timeout=30
quit
"""


def compare(a: Path, b: Path) -> dict:
    """The two games' telemetry over the ticks both have (each quits on its own a few ticks after --ticks), line for
    line: equal, or the first tick that differs and how."""
    if not a.exists() or not b.exists():
        return {"equal": False, "why": "telemetry missing"}
    # A game killed at the end may leave its last line half written.
    whole = lambda text: text.splitlines() if text.endswith("\n") else text.splitlines()[:-1]  # noqa: E731
    la, lb = whole(a.read_text()), whole(b.read_text())
    n = min(len(la), len(lb))
    for i in range(n):
        if la[i] != lb[i]:
            ja, jb = json.loads(la[i]), json.loads(lb[i])
            return {"equal": False, "lines": [len(la), len(lb)], "first_diff_tick": ja.get("tick"),
                    "host": ja, "join": jb}
    return {"equal": n > 0, "lines": [len(la), len(lb)], "common_to_tick": json.loads(la[n - 1]).get("tick") if n else None}


def orders(run: Path) -> list:
    """The commands a game executed (packets.jsonl without empty turns and ctMachineSettings), without the slots'
    stale tails, which differ between machines."""
    if not (run / "packets.jsonl").exists():
        return []
    out = []
    for line in (run / "packets.jsonl").read_text().splitlines():
        p = json.loads(line)
        if p["type"] not in (0, 52):
            out.append((p["tick"], p["player"], p["type"], p["data"]))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--ticks", type=int, default=3000, help="play to this tick (a multiple of 500), then quit")
    ap.add_argument("--script0", type=Path, help="the host's (player 0's) tick script")
    ap.add_argument("--script1", type=Path, help="the joiner's (player 1's) tick script")
    ap.add_argument("--telemetry-every", type=int, default=1)
    ap.add_argument("--turbo", action="store_true", help="uncapped game speed (shell.py --turbo)")
    ap.add_argument("--timeout", type=float, default=900, help="wall-clock limit of each game")
    args = ap.parse_args()
    if args.ticks % 500:
        ap.error("--ticks must be a multiple of 500 (the shell's `game` tick events)")
    prefix = os.environ.get("OP2_HARNESS_PREFIX", "")
    shared = {(HERE.parent / p).resolve() for p in ("prefix", "prefix-harness")}
    if not prefix or Path(prefix).resolve() in shared:
        ap.error("set OP2_HARNESS_PREFIX to a prefix of its own (e.g. prefix-net, a copy of prefix-harness), "
                 "never prefix/ (David's) or prefix-harness/ (shared by every harness run)")
    out = args.out.resolve()
    sync = out / "sync"
    out.mkdir(parents=True, exist_ok=True)
    for f in sync.glob("*") if sync.exists() else []:
        f.unlink()
    game_timeout = int(args.timeout)
    for name, text in (("host", HOST), ("join", JOIN)):
        (out / f"{name}_drive.txt").write_text(text.format(ticks=args.ticks, game_timeout=game_timeout))

    def shell(name: str, script: Path | None) -> list[str]:
        cmd = [sys.executable, str(HERE / "shell.py"), "--net", "--drive", str(out / f"{name}_drive.txt"), "--sync", str(sync),
               "--telemetry-every", str(args.telemetry_every), "--ini", "IntroAVI=0", "--ini", "Music=0", "--ini", "Sound=0",
               "--timeout", str(args.timeout + 200), "--out", str(out / name)]
        if args.turbo:
            cmd += ["--turbo", "--draw-from", str(args.ticks)]
        if script:
            cmd += ["--script", str(script.resolve())]
        return cmd

    t0 = time.monotonic()
    # SIGTERM (`timeout`) raises like Ctrl+C, so the finally stops both shell.py runs, which end their games and Xvfb.
    signal.signal(signal.SIGTERM, lambda signum, _frame: sys.exit(128 + signum))
    procs: list[subprocess.Popen] = []
    try:
        with open(out / "join.log", "w") as jl, open(out / "host.log", "w") as hl:
            procs.append(subprocess.Popen(shell("join", args.script1), stdout=jl, stderr=subprocess.STDOUT))
            procs.append(subprocess.Popen(shell("host", args.script0), stdout=hl, stderr=subprocess.STDOUT))
            join, host = procs
            rc = {"host": host.wait(), "join": join.wait()}
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()  # shell.py's own SIGTERM handling kills its game's process group and Xvfb
        for p in procs:
            try:
                p.wait(timeout=20)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
    tel = compare(out / "host" / "telemetry.jsonl", out / "join" / "telemetry.jsonl")
    oh, oj = orders(out / "host"), orders(out / "join")
    n = min(len(oh), len(oj))
    same_orders = oh[:n] == oj[:n]
    summary = {"exit": rc, "wall_seconds": round(time.monotonic() - t0, 1), "ticks": args.ticks, "turbo": args.turbo,
               "orders": {"host": len(oh), "join": len(oj), "same": same_orders}, "telemetry": tel}
    (out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    brief = {k: v for k, v in tel.items() if k not in ("host", "join")}
    print(json.dumps({"exit": rc, "wall_seconds": summary["wall_seconds"], "orders": summary["orders"], "telemetry": brief}))
    return 0 if rc == {"host": 0, "join": 0} and tel.get("equal") and same_orders else 1


if __name__ == "__main__":
    sys.exit(main())
