#!/usr/bin/env python3
"""Check of the play-and-record mode (`run.sh --record`, README "Recording your own play"): plays a short session of a
mission with real X input under a private Xvfb, the way a person would (select units, right-click orders, SAVE GAME,
more orders, LOAD GAME, different orders), lets it end at a tick limit, then replays the recording it produced with the
normal harness and compares:
  - the replay's executed packets (harness.log `packet:` lines) with the recording's `packet` lines: equal;
  - the replay's telemetry and trace with the recorded session's (trace without the draw-dependent frame/anim): equal;
  - the packets of the history abandoned by the load: in the session's harness.log, not in the recording.
Needs Xvfb and Wine (sandbox disabled) and the installed harness; uses prefix-harness's OPU/saves slot it saves to
(SAVE and LOAD pick the slot the dialogs open on) and restores nothing else. Takes about 2 minutes (real-time play).

    python3 test/record_check.py --out /tmp/reccheck
    python3 test/record_check.py --steps FILE --out DIR   # explore: run other driver steps, no replay

Driver steps (one per line, `#` comments): `tick T` (wait until the game reaches tick T), `sleep S`, `shot NAME` (the
X screen to NAME.png), `click X Y [right]`, `key KEYSYM...`, `type TEXT` (letters and spaces). Pixels are X screen
pixels; the game window's client area starts at CLIENT (printed at the start; harness.log's `window` line).
"""
import argparse
import json
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))
import run  # noqa: E402
from shell import SavesAside, X  # noqa: E402

# The built-in session on e01, in X screen pixels of a 1024x768 game window at the top left of a private Xvfb without
# a window manager (client area from (4, 30)). Classic controls: a left click selects a unit, a left click on the
# ground moves the selection there. The command pane's sixth tab (1027, 174) is Game Options: LOAD GAME (948, 315),
# SAVE GAME (948, 340); in both dialogs the first slot is at (397, 281) and SAVE/LOAD at (612, 482) (the folder is
# empty, so SAVE opens on the first slot and LOAD on the only save). The game is paused while a dialog is up.
STEPS = """
tick 60             # the mission's view is on the base (the pointer waits over the command pane: off the detail
click 507 437       # pane the view scrolls); an Evacuation Transport
tick 70
click 700 300       # move it
tick 100
click 444 503       # another one
tick 110
click 650 420
tick 200
click 1027 174      # Game Options, SAVE GAME, a name, SAVE
tick 210
click 948 340
sleep 1.5
click 397 281
type rec one
sleep 0.3
shot save_dialog
click 612 482
sleep 1.5
tick 230            # the history the load abandons: two ConVecs moved
click 443 630
tick 240
click 700 650
tick 300
click 410 662
tick 310
click 760 560
tick 400
click 1027 174      # Game Options, LOAD GAME, LOAD
tick 410
click 948 315
sleep 1.5
shot load_dialog
click 612 482
sleep 2
shot loaded
tick 240            # different play after the load
click 410 662
tick 250
click 600 200
tick 300
shot end
"""


def wait_tick(out: Path, tick: int, proc: subprocess.Popen, timeout: float = 120) -> int:
    """Waits until telemetry.jsonl reaches `tick` (its lines come every --telemetry-every ticks)."""
    end = time.monotonic() + timeout
    telem = out / "telemetry.jsonl"
    while time.monotonic() < end:
        if proc.poll() is not None:
            sys.exit(f"the game ended while waiting for tick {tick}")
        last = 0
        if telem.exists():
            lines = telem.read_bytes().splitlines()
            for line in reversed(lines):
                try:
                    last = json.loads(line)["tick"]
                    break
                except (json.JSONDecodeError, KeyError):
                    continue
        if last >= tick:
            return last
        time.sleep(0.05)
    sys.exit(f"timed out waiting for tick {tick}")


def drive(x: X, steps: str, out: Path, proc: subprocess.Popen, display: str):
    for raw in steps.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        w = line.split()
        print(f"  step: {line}", flush=True)
        if w[0] == "tick":
            wait_tick(out, int(w[1]), proc)
        elif w[0] == "sleep":
            time.sleep(float(w[1]))
        elif w[0] == "shot":
            run.grab_screen(display, out / f"{w[1]}.png")
        elif w[0] == "click":
            x.click(int(w[1]), int(w[2]), 3 if w[3:4] == ["right"] else 1)
            time.sleep(0.15)
        elif w[0] == "key":
            for k in w[1:]:
                x.key(k)
                time.sleep(0.1)
        elif w[0] == "type":
            for ch in line.split(None, 1)[1]:
                x.key("space" if ch == " " else ch)
                time.sleep(0.05)
        else:
            sys.exit(f"unknown step: {line}")


def record(args, out: Path, steps: str) -> int:
    """The recorded session: Xvfb, run.py --record on it, the driver steps, with an empty save folder (put back after;
    the session's saves and their sidecars are kept in OUT/saves). Returns run.py's exit status."""
    width, height = 1024, 768
    saves = SavesAside(run.OPU_DIR / "saves", out)
    xvfb, display = run.start_xvfb(width, height)
    try:
        cmd = [str(HERE / "run.sh"), "--record", "--silent", "--display", display, "--mission", args.mission,
               "--ticks", str(args.ticks), "--telemetry-every", "10", "--trace-every", "4", "--out", str(out)]
        if args.campaign:
            cmd += ["--campaign", str(args.campaign)]
        if args.desktop:
            cmd += ["--desktop", args.desktop]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True,
                                env=dict(run.os.environ, OP2H_SAVES_LOCK_HELD="1"))
        try:
            x = X(display)
            x.move(950, 650)  # Over the empty part of the command pane: anywhere off the detail pane scrolls the view.
            wait_tick(out, 1, proc)
            drive(x, steps, out, proc, display)
            code = proc.wait(timeout=600)
        finally:
            if proc.poll() is None:
                run.os.killpg(proc.pid, signal.SIGKILL)
        print(proc.stdout.read().strip().splitlines()[-1] if proc.stdout else "")
        return code
    finally:
        xvfb.terminate()
        xvfb.wait()
        saves.restore()


def _terminate(signum, _frame):
    raise SystemExit(128 + signum)  # So `timeout` still runs the cleanup (the game, Xvfb).


def main() -> int:
    signal.signal(signal.SIGTERM, _terminate)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--mission", default="e01.dll")
    ap.add_argument("--campaign", type=Path)
    ap.add_argument("--ticks", type=int, default=700, help="the recorded session ends at this tick (0: the mission's end)")
    ap.add_argument("--steps", type=Path, help="other driver steps (explore: no replay or checks)")
    ap.add_argument("--input", action="store_true", help="check a setup script with input verbs in a recording instead")
    ap.add_argument("--desktop", metavar="WxH", help="play in a Wine virtual desktop, as `make -C missions record` does")
    ap.add_argument("--script", type=Path, help="fast mode: a win script played by a `--record --fast` session ...")
    ap.add_argument("--loads", metavar="S:L,...", help="... that saves at tick S and loads that save at tick L (pairs "
                    "in order); its replay must match it: saves and loads are faithful for that mission")
    args = ap.parse_args()
    out = args.out.resolve()
    if args.script:
        return fast_check(args, out)
    if args.input:
        return input_check(out)
    rec = out / "rec"
    steps = args.steps.read_text() if args.steps else STEPS
    code = record(args, rec, steps)
    print(f"recorded session: exit {code}, {rec / 'recording.txt'}")
    if args.steps:
        return code
    assert code == 0, "the recorded session failed"
    return check(args, out, rec)


INPUT_SETUP = """0 coords file
20 select 43          # an Evacuation Transport (e01's ids)
30 click tile 25 10   # moves it: the UI's Move packet, through the harness's virtual cursor
"""


def strip_input_state(path: Path) -> list:
    """Telemetry without `view` and `selected`: after a recording's setup input the player's own view and selection."""
    lines = []
    for line in path.read_text().splitlines():
        t = json.loads(line)
        t.pop("view", None)
        t.pop("selected", None)
        lines.append(t)
    return lines


def input_check(out: Path) -> int:
    """A setup script with input verbs in a `--record --fast` session runs as in a plain run (the virtual cursor and
    keys are active while they run): the same telemetry, the UI's packet recorded once, and the recording replays."""
    out.mkdir(parents=True, exist_ok=True)
    setup = out / "input_setup.txt"
    setup.write_text(INPUT_SETUP)
    common = ["--mission", "e01.dll", "--ticks", "300", "--telemetry-every", "10", "--trace-every", "4"]
    runs = {}
    for name, extra in (("rec", ["--record", "--fast", "--script", str(setup)]), ("plain", ["--script", str(setup)])):
        p = subprocess.run([str(HERE / "run.sh"), *extra, *common, "--out", str(out / name)], capture_output=True, text=True)
        assert p.returncode == 0, p.stdout + p.stderr
        runs[name] = out / name
    recorded = [line for line in (runs["rec"] / "recording.txt").read_text().splitlines() if re.match(r"^\d+ packet ", line)]
    assert len(recorded) == 1, ("the setup's click should give one Move packet", recorded)
    assert strip_input_state(runs["rec"] / "telemetry.jsonl") == strip_input_state(runs["plain"] / "telemetry.jsonl"), \
        "the recording session's setup input didn't play as in a plain run"
    p = subprocess.run([str(HERE / "run.sh"), "--script", str(runs["rec"] / "recording.txt"), *common, "--out", str(out / "replay")],
                       capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    assert strip_input_state(out / "replay" / "telemetry.jsonl") == strip_input_state(runs["rec"] / "telemetry.jsonl"), "replay differs"
    print(f"input check: the setup's click recorded once ({recorded[0].split('#')[0].strip()}); session = plain run = replay "
          "(telemetry without view and selection)")
    return 0


def fast_check(args, out: Path) -> int:
    """A `--record --fast` session of a win script with saves and loads from the script, against the plain replay of
    the recording it wrote (the win script): the same telemetry (--eco, every 10 ticks) means the game after each load
    went on exactly as the game that was saved would have."""
    out.mkdir(parents=True, exist_ok=True)
    lines = [args.script.read_text().rstrip("\n")]
    names = []
    for k, pair in enumerate((args.loads or "").split(",") if args.loads else []):
        s, l = (int(v) for v in pair.split(":"))
        names.append(f"reccheck{run.os.getpid()}_{k}.op2")
        lines += [f"{s} savegame {names[-1]}", f"{l} loadgame {names[-1]}"]
    setup = out / "setup.txt"
    setup.write_text("\n".join(lines) + "\n")
    common = ["--mission", args.mission, "--ticks", str(args.ticks), "--eco", "--telemetry-every", "10"]
    if args.campaign:
        common += ["--campaign", str(args.campaign)]
    rec, replay = out / "rec", out / "replay"
    try:
        p = subprocess.run([str(HERE / "run.sh"), "--record", "--fast", "--script", str(setup), *common, "--out", str(rec)],
                           capture_output=True, text=True)
        assert p.returncode == 0, p.stdout + p.stderr
    finally:
        for n in names:
            (run.OPU_DIR / "saves" / n).unlink(missing_ok=True)
            run.shutil.rmtree(run.OPU_DIR / "saves" / f"{n}.rec", ignore_errors=True)
    text = (rec / "recording.txt").read_text()
    loads = [line for line in text.splitlines() if line.startswith("# load ")]
    assert len(loads) == len(names) and "BROKEN" not in text, text
    p = subprocess.run([str(HERE / "run.sh"), "--script", str(rec / "recording.txt"), *common, "--out", str(replay)],
                       capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    a, b = rec / "telemetry.jsonl", replay / "telemetry.jsonl"
    end = json.loads(a.read_text().splitlines()[-1])
    # Every unit's MapObject +0x7C is a heap address (0x06xxxxxx, 0x78 apart from unit to unit: a per-unit record) that a
    # loaded game allocates elsewhere: masked. Everything else is compared; a structure hive's slot number in
    # HiveManager's array (`hive`'s last value) is reported on its own, with the ticks it differs over, since a
    # different slot is a sign that the loaded game isn't the continued one, even when nothing else ever differs.
    ta, tb = a.read_text().splitlines(), b.read_text().splitlines()
    slot_ticks, other = [], None
    for la, lb in zip(ta, tb):
        if la == lb:
            continue
        ja, jb = json.loads(la), json.loads(lb)
        for t in (ja, jb):
            for u in t["units"]:
                if "raw" in u:
                    raw = bytearray.fromhex(u["raw"])
                    raw[0x7C - 0x48:0x80 - 0x48] = bytes(4)
                    u["raw"] = raw.hex()
        ua = {u["id"]: u for u in ja["units"]}
        ub = {u["id"]: u for u in jb["units"]}
        if any("hive" in u and ub.get(i, {}).get("hive", [None])[-1:] != u["hive"][-1:] for i, u in ua.items()):
            slot_ticks.append(ja["tick"])
        for t in (ja, jb):
            for u in t["units"]:
                if "hive" in u:
                    u["hive"] = u["hive"][:-1]
        if ja != jb and other is None:
            other = ja["tick"]
            diff = [f"unit {i} {k}: {u.get(k)} / {ub.get(i, {}).get(k)}" for i, u in ua.items() for k in u
                    if u.get(k) != ub.get(i, {}).get(k)][:3] or [k for k in ja if ja[k] != jb.get(k)][:3]
            other = f"tick {ja['tick']}: {'; '.join(map(str, diff))}"
    if len(ta) != len(tb) and other is None:
        other = f"{len(ta)} lines against {len(tb)}"
    slots = f"; the hive slot numbers differ at {len(slot_ticks)} samples, ticks {slot_ticks[0]}-{slot_ticks[-1]}" if slot_ticks else ""
    print(f"{args.mission}: {len(names)} loads ({'; '.join(l.split(': ', 1)[1].split(',')[0] for l in loads)}); the session ended at "
          f"tick {end['tick']} (end {end['end']}); telemetry (--eco, every 10 ticks, {len(ta)} lines, +0x7C masked) "
          f"{'DIFFERS at ' + other if other else 'IDENTICAL'} to the replay's{slots}")
    return 1 if other else 0


def packet_lines(log: Path) -> list[str]:
    """`tick type hex` of the harness.log `packet:` lines (the local player's executed packets)."""
    out = []
    for line in log.read_text().splitlines():
        if line.startswith("packet: tick "):
            w = line.split()
            out.append(f"{w[2]} {w[4]} {w[5]}")
    return out


def check(args, out: Path, rec: Path) -> int:
    recording = rec / "recording.txt"
    text = recording.read_text()
    recorded = [" ".join(line.split("#")[0].split()[:4:]).replace(" packet", "").split(" +")[0] for line in text.splitlines()
                if re.match(r"^\d+ packet ", line)]
    session = packet_lines(rec / "harness.log")
    assert "# load " in text and "BROKEN" not in text, "the session's load isn't in the recording"
    abandoned = [p for p in session if p not in recorded]
    assert recorded and abandoned, ("the session should have kept and abandoned packets", recorded, session)
    print(f"recording: {len(recorded)} packets; the load dropped {len(abandoned)}: {abandoned}")

    replay = out / "replay"
    cmd = [str(HERE / "run.sh"), "--mission", args.mission, "--script", str(recording), "--ticks", str(args.ticks),
           "--telemetry-every", "10", "--trace-every", "4", "--out", str(replay)]
    if args.campaign:
        cmd += ["--campaign", str(args.campaign)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    replayed = packet_lines(replay / "harness.log")
    assert replayed == recorded, ("replayed packets differ", recorded, replayed)
    assert "WARN" not in (replay / "harness.log").read_text(), "the replay warned"
    telem_rec, telem_rep = (d / "telemetry.jsonl" for d in (rec, replay))
    assert telem_rec.read_bytes() == telem_rep.read_bytes(), "telemetry differs"
    # The trace's frame/anim are what the renderer last drew: the recording draws every frame, the replay hardly any.
    def trace(d: Path) -> list:
        lines = [json.loads(line) for line in (d / "trace.jsonl").read_text().splitlines()]
        for t in lines:
            for u in t.get("units", []):
                u.pop("frame", None)
                u.pop("anim", None)
        return lines
    tr_rec, tr_rep = trace(rec), trace(replay)
    assert tr_rec == tr_rep, "trace differs"
    n = len(telem_rec.read_text().splitlines())
    print(f"replay: same packets, telemetry identical ({n} lines, to tick {args.ticks}), trace identical "
          f"({len(tr_rec)} lines, frame/anim aside)")

    # op2-inspect's game_start takes the recording as a session's base script and plays on from its end.
    inspect = HERE.parent / "tools" / "target" / "debug" / "op2-inspect"
    if not inspect.is_file():
        print("op2-inspect isn't built (cd tools && cargo build -p op2-inspect): game_start not checked")
    else:
        env = dict(run.os.environ, TMPDIR=str(out / "tmp"), OP2_INSPECT_HOME=str(out / "inspect"))
        (out / "tmp").mkdir(exist_ok=True)
        start = ["game_start", f"mission={Path(args.mission).stem}", f"script={recording}", f"tick={args.ticks}", "name=reccheck"]
        start.append(f"campaign={args.campaign}" if args.campaign else "campaign=none")
        g = subprocess.run([str(inspect), *start], capture_output=True, text=True, env=env, cwd=HERE.parent)
        assert g.returncode == 0 and f"at tick {args.ticks}" in g.stdout, g.stdout + g.stderr
        session_state = out / "inspect" / "game" / "reccheck" / "run" / "state.json"
        if session_state.is_file():
            st = json.loads(session_state.read_text())
            final = json.loads(telem_rec.read_text().splitlines()[-1])
            pad = st["map"]["pad"]
            ours = sorted((u["id"], u["x"], u["y"]) for u in st["units"] if u["owner"] == 0)
            theirs = sorted((u["id"], u["x"] - pad, u["y"]) for u in final["units"] if u["owner"] == 0)
            assert st["tick"] == final["tick"] and ours == theirs, (st["tick"], final["tick"], ours, theirs)
            print(f"game_start: the session starts where the recording ends, tick {st['tick']}, the same {len(ours)} "
                  "units of player 0 on the same tiles")
        else:
            print(f"game_start: started at tick {args.ticks}")
        subprocess.run([str(inspect), "game_stop"], capture_output=True, text=True, env=env, cwd=HERE.parent)
    print("record check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
