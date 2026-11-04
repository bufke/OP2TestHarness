#!/usr/bin/env python3
"""Play a LAN game of the original between two to six copies of the game under Wine on this machine, and log it from
inside every copy (docs/multiplayer.md "Stage 2a").

    OP2_HARNESS_PREFIX=$PWD/prefix-net harness/netgame.py --out /tmp/net --ticks 3000 \\
        --script0 harness/oracle/net/ml2_08_p0.txt --script1 harness/oracle/net/ml2_08_p1.txt --turbo
    OP2_HARNESS_PREFIX=$PWD/prefix-net harness/netgame.py --out /tmp/net --mission "Midas.*'Hidden" --seed 7 \\
        --players 3 --eden 0,2 --ticks 6000 --turbo --script0 ... --script1 ... --script2 ...

Shell-mode runs (shell.py --net) boot the game normally in their own Xvfb and Wine virtual desktop, in the same prefix
and wineserver: the host clicks MULTIPLAYER > INTERNET (TCP/IP) > CREATE, picks the Max Players (`--players`) and the
Scenario Type (`--type`, a regex: Last One Standing, its default, Midas, Resource Race, Space Race, Land Rush), OK, then
in GAME SETTINGS the scenario (`--mission`, a regex on the list of that type, e.g. "2P, LoS, 'Ambush Alley'", its
default), the Initial Vehicles (`--vehicles`) and its Eden box; each joiner in turn clicks
MULTIPLAYER > INTERNET > JOIN > OK, types 127.0.0.1, picks the session, ticks its own Eden box and Ready; the host
presses START. The drivers wait for each other through files in OUT/sync (shell.py `touch` and `waitfile`): joiner k
joins after joiner k-1 is ready, so player k is joiner k. Each game's tick script gives its own player's orders,
which go through the original's network turns (harness.cpp "Network games"). `--seed N` makes every copy start from
seed N instead of the lobby's (harness.cpp OP2H_NET_SEED), so a game can be recorded again from its recipe.

Outputs: OUT/host, OUT/join, OUT/join2, ... each with start_info.json, packets.jsonl, net.jsonl, telemetry.jsonl and
the shell run's files; OUT/summary.json says whether every copy executed the same orders and kept the same telemetry,
line for line, as the host, and where they first differ.

Use a prefix nobody else is running (prefix-net, a copy of prefix-harness: tools that run harness/run.sh share
prefix-harness, and its wineserver would host the games). It refuses prefix/ and prefix-harness/.
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
{create}ctl id=1
sleep 4
{pick}{vehicles}{eden}dump lobby
touch host_lobby
{wait_joiners}sleep 3
shot lobby_ready
ctl id=1045
wait game what=first_cycle timeout=60
touch host_game
wait game what=tick tick={ticks} timeout={game_timeout}
shot game_end
touch host_done
{wait_done}quit
"""

JOIN = """waitfile {after} timeout={lobby_timeout}
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
{eden}ctl id={ready}
sleep 1
shot lobby_joined
touch {name}_ready
wait game what=first_cycle timeout=90
wait game what=tick tick={ticks} timeout={game_timeout}
shot game_end
touch {name}_done
waitfile host_done timeout=30
quit
"""


def side_name(player: int) -> str:
    """The output folder of player N's copy: host, join, join2, ... (op2-missionhost netlog's SIDES)."""
    return "host" if player == 0 else "join" if player == 1 else f"join{player}"


def seat_name(player: int) -> str:
    """Player N's name in the lobby (its private ini's [Game] Name): Host, Joiner1, Joiner2, ..., distinct so a chat
    line's or a message's sender can be told apart (net_chat)."""
    return "Host" if player == 0 else f"Joiner{player}"


def lobby_ids(player: int) -> tuple[int, int]:
    """The GAME SETTINGS dialog's Eden and Ready check boxes of the player's row: 40003/40004 on the host's row, then
    40006/40007, 40011/40012, ... five ids a row (the lobby dump in net_ml2_08's shell.jsonl)."""
    return (40003, 40004) if player == 0 else (40006 + 5 * (player - 1), 40007 + 5 * (player - 1))


# Telemetry fields of one machine's interface (the detail view's corner, the selection), not of the game.
LOCAL_UI = ("view", "selected")


def compare(a: Path, b: Path) -> dict:
    """Two games' telemetry over the ticks both have (each quits on its own a few ticks after --ticks), line for
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
            # A script with input verbs (chat: `key enter`, `type`) adds each machine's own view and selection.
            if {k: v for k, v in ja.items() if k not in LOCAL_UI} == {k: v for k, v in jb.items() if k not in LOCAL_UI}:
                continue
            return {"equal": False, "lines": [len(la), len(lb)], "first_diff_tick": ja.get("tick"),
                    "host": ja, "other": jb}
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


def pacing(run: Path) -> list:
    """The copy's game speed (GameImpl::gameSpeed_, shell.jsonl's `speed` events) over the game, each span with the
    ticks per wall second it ran at, from the ticks and times of the datagrams it sent (net.jsonl: one or more a turn).
    A span's first and last 40 ticks are left out (a change takes a turn or two to show in the limiter)."""
    def lines(name: str):
        """A log's JSON lines, skipping any a killed game left half written."""
        for line in (run / name).read_text().splitlines() if (run / name).exists() else []:
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue
    changes = [(e["tick"], e["speed"]) for e in lines("shell.jsonl") if e.get("what") == "speed" and "tick" in e]
    sent = [(d["tick"], d["ms"]) for d in lines("net.jsonl") if d.get("dir") == "send" and d.get("tick", 0) > 0 and "ms" in d]
    spans = []
    for i, (t0, speed) in enumerate(changes):
        t1 = changes[i + 1][0] if i + 1 < len(changes) else (sent[-1][0] if sent else t0)
        inside = [(t, ms) for t, ms in sent if t0 + 40 <= t <= t1 - 40]
        tps = None
        if len(inside) >= 2 and inside[-1][1] > inside[0][1]:
            tps = round((inside[-1][0] - inside[0][0]) * 1000 / (inside[-1][1] - inside[0][1]), 2)
        spans.append({"from": t0, "to": t1, "speed": speed, "ticks_per_s": tps})
    return spans


def speed_problems(pacing: dict, speeds: list) -> list:
    """How the copies' game speeds disagree: each copy's (tick, speed) changes must be the host's, and with `--speed`
    the start must be the host's preference x 4. Empty: one speed for every machine, as the original keeps."""
    changes = {side: [(s["from"], s["speed"]) for s in spans] for side, spans in pacing.items()}
    host = changes.get("host", [])
    bad = [] if host else ["host: no speed events"]
    bad += [f"{side}: speed changes {c}, the host's {host}" for side, c in changes.items() if side != "host" and c != host]
    if speeds and host and host[0][1] != 4 * speeds[0]:
        bad.append(f"the start's speed {host[0][1]} isn't the host's preference {speeds[0]} x 4")
    return bad


def start_problems(out: Path, args, eden: set) -> list:
    """How each copy's start_info.json differs from what was asked (seed, mission DLL, players, Eden boxes, initial
    vehicles): a lobby step that missed would otherwise record another game under the same recipe. Empty: as asked."""
    bad = []
    for p in range(args.players):
        f = out / side_name(p) / "start_info.json"
        if not f.exists():
            bad.append(f"{side_name(p)}: no start_info.json")
            continue
        s = json.loads(f.read_text())
        want = {"humanPlayers": args.players, "seed": args.seed, "script": args.dll, "initialVehicles": args.vehicles}
        bad += [f"{side_name(p)}: {k} {s.get(k)!r}, not {v!r}" for k, v in want.items()
                if v is not None and str(s.get(k)).lower() != str(v).lower()]
        colonies = [int(slot.get("eden", 0)) for slot in s.get("slots", [])[:args.players]]
        if colonies != [int(i in eden) for i in range(args.players)]:
            bad.append(f"{side_name(p)}: Eden slots {colonies}, not {sorted(eden)}")
    return bad


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--ticks", type=int, default=3000, help="play to this tick (a multiple of 500), then quit")
    ap.add_argument("--players", type=int, default=2, help="copies of the game, 2 to 6 (the scenario must take them)")
    for p in range(6):
        ap.add_argument(f"--script{p}", type=Path, help=f"player {p}'s tick script" + (" (the host)" if p == 0 else ""))
    ap.add_argument("--type", help="the scenario type: a regex on NEW NETWORK GAME's list (Last One Standing, Midas, "
                                   "Resource Race, Space Race, Land Rush; default its own, Last One Standing)")
    ap.add_argument("--mission", help="the lobby's scenario: the first entry of its list matching this regex "
                                      "(case-insensitive; default: the lobby's own, 2P LoS 'Ambush Alley')")
    ap.add_argument("--eden", default="", help="players who tick Eden in the lobby, e.g. 0,2 (default: all Plymouth)")
    ap.add_argument("--seed", type=int, help="every copy starts from this seed instead of the lobby's")
    ap.add_argument("--dll", help="the mission DLL the scenario must be (e.g. mm2_07.dll): checked in every copy's "
                                  "start_info.json with the seed, players, Eden boxes and vehicles")
    ap.add_argument("--vehicles", type=int, help="the lobby's Initial Vehicles (default: its own, 0)")
    ap.add_argument("--telemetry-every", type=int, default=1)
    ap.add_argument("--speed", help="each copy's Game Speed preference (outpost2.ini GameSpeed, 1-10), in player "
                                    "order, e.g. 10,3 (one value: every copy; default the prefix's ini)")
    ap.add_argument("--turbo", action="store_true", help="uncapped game speed (shell.py --turbo)")
    ap.add_argument("--timeout", type=float, default=900, help="wall-clock limit of each game")
    args = ap.parse_args()
    if args.ticks % 500:
        ap.error("--ticks must be a multiple of 500 (the shell's `game` tick events)")
    if not 2 <= args.players <= 6:
        ap.error("--players takes 2 to 6")
    try:
        speeds = [int(s) for s in args.speed.split(",")] if args.speed else []
    except ValueError:
        ap.error(f"--speed takes numbers 1-10 separated by commas, not {args.speed!r}")
    if len(speeds) == 1:
        speeds *= args.players
    if speeds and (len(speeds) != args.players or not all(1 <= s <= 10 for s in speeds)):
        ap.error("--speed takes one value 1-10, or one per player")
    eden = {int(p) for p in args.eden.split(",") if p.strip()}
    if any(p >= args.players for p in eden):
        ap.error("--eden names a player past --players")
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
    joiners = [side_name(p) for p in range(1, args.players)]
    # Joiners join one after another; each waits up to two minutes per earlier one.
    lobby_timeout = 120 * args.players
    pick = f"pick id=40032 {args.mission.replace(' ', '_')}\nsleep 2\n" if args.mission else ""
    # NEW NETWORK GAME: Max Players (1015) and Scenario Type (1039: Last One Standing, Midas, Resource Race, Space Race,
    # Land Rush), which decides the lobby's scenario list.
    create = (f"pick id=1015 ^{args.players}$\nsleep 0.5\n" if args.players != 2 else "") + \
        (f"pick id=1039 {args.type.replace(' ', '_')}\nsleep 0.5\n" if args.type else "")
    drives = {"host": HOST.format(
        ticks=args.ticks, game_timeout=game_timeout, pick=pick, create=create,
        vehicles=f"pick id=1018 ^{args.vehicles}$\nsleep 1\n" if args.vehicles is not None else "",
        eden=f"ctl id={lobby_ids(0)[0]}\nsleep 1\n" if 0 in eden else "",
        wait_joiners="".join(f"waitfile {j}_ready timeout={lobby_timeout}\n" for j in joiners),
        wait_done="".join(f"waitfile {j}_done timeout=30\n" for j in joiners))}
    for p in range(1, args.players):
        e, ready = lobby_ids(p)
        drives[side_name(p)] = JOIN.format(
            ticks=args.ticks, game_timeout=game_timeout, lobby_timeout=lobby_timeout, name=side_name(p), ready=ready,
            after="host_lobby" if p == 1 else f"{side_name(p - 1)}_ready",
            eden=f"ctl id={e}\nsleep 1\n" if p in eden else "")
    for name, text in drives.items():
        (out / f"{name}_drive.txt").write_text(text)

    def shell(p: int, script: Path | None) -> list[str]:
        name = side_name(p)
        cmd = [sys.executable, str(HERE / "shell.py"), "--net", "--drive", str(out / f"{name}_drive.txt"), "--sync", str(sync),
               "--telemetry-every", str(args.telemetry_every), "--ini", "IntroAVI=0", "--ini", "Music=0", "--ini", "Sound=0",
               "--ini", f"Name={seat_name(p)}",
               "--timeout", str(args.timeout + 200 + lobby_timeout), "--out", str(out / name)]
        if speeds:
            cmd += ["--ini", f"GameSpeed={speeds[p]}"]
        if args.turbo:
            cmd += ["--turbo", "--draw-from", str(args.ticks)]
        if args.seed is not None:
            cmd += ["--net-seed", str(args.seed)]
        if script:
            cmd += ["--script", str(script.resolve())]
        return cmd

    t0 = time.monotonic()
    # SIGTERM (`timeout`) raises like Ctrl+C, so the finally stops every shell.py run, which end their games and Xvfb.
    signal.signal(signal.SIGTERM, lambda signum, _frame: sys.exit(128 + signum))
    procs: dict[str, subprocess.Popen] = {}
    logs = []
    try:
        # Joiners first (they wait for the host's lobby), the host last.
        for p in [*range(1, args.players), 0]:
            name = side_name(p)
            logs.append(open(out / f"{name}.log", "w"))
            procs[name] = subprocess.Popen(shell(p, getattr(args, f"script{p}")), stdout=logs[-1], stderr=subprocess.STDOUT)
        rc = {name: procs[name].wait() for name in ["host", *joiners]}
    finally:
        for p in procs.values():
            if p.poll() is None:
                p.terminate()  # shell.py's own SIGTERM handling kills its game's process group and Xvfb
        for p in procs.values():
            try:
                p.wait(timeout=20)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        for f in logs:
            f.close()
    host_orders = orders(out / "host")
    tel, ords = {}, {"host": len(host_orders)}
    same_orders = True
    for j in joiners:
        tel[j] = compare(out / "host" / "telemetry.jsonl", out / j / "telemetry.jsonl")
        oj = orders(out / j)
        n = min(len(host_orders), len(oj))
        ords[j] = len(oj)
        same_orders = same_orders and host_orders[:n] == oj[:n]
    ords["same"] = same_orders
    # Two players: the old summary's single comparison; more: one per joiner against the host.
    telemetry = tel["join"] if args.players == 2 else tel
    summary = {"exit": rc, "wall_seconds": round(time.monotonic() - t0, 1), "ticks": args.ticks, "turbo": args.turbo,
               "players": args.players, "type": args.type, "mission": args.mission, "seed": args.seed, "eden": sorted(eden), "vehicles": args.vehicles,
               "dll": args.dll, "orders": ords, "telemetry": telemetry, "start": start_problems(out, args, eden),
               "speed": speeds or None, "pacing": {side_name(p): pacing(out / side_name(p)) for p in range(args.players)}}
    # Every copy's speed changes at the same ticks to the same speeds (the start's is the host's; `speed_problems`).
    summary["speed_problems"] = speed_problems(summary["pacing"], speeds)
    (out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    brief = {j: {k: v for k, v in t.items() if k not in ("host", "other")} for j, t in tel.items()}
    print(json.dumps({"exit": rc, "wall_seconds": summary["wall_seconds"], "orders": ords, "telemetry": brief, "start": summary["start"],
                      "pacing": summary["pacing"]}))
    if summary["speed_problems"]:
        print("speed:", "; ".join(summary["speed_problems"]))
    ok = all(v == 0 for v in rc.values()) and all(t.get("equal") for t in tel.values()) and same_orders and not summary["start"] \
        and not summary["speed_problems"]
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
