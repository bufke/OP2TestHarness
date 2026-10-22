#!/usr/bin/env python3
"""Self-test of the session state (`run.sh --state`, `--stop`) and of op2-inspect's game_* tools on the self-test
mission zHarnessEnd.dll (a Command Center and two Lynx; "Survive until tick 800"). Run by `make -C harness check`
after the plain check, while the test mission is installed; build/check is that check's run.

The game_* part runs the op2-inspect CLI (tools/target/debug/op2-inspect) with TMPDIR inside build/, so it never
touches a session of the user's; it is skipped with a note when op2-inspect isn't built.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
BUILD = HERE / "build"
INSPECT = HERE.parent / "tools" / "target" / "debug" / "op2-inspect"


def run(*args: str) -> dict:
    out = subprocess.run([str(HERE / "run.sh"), "--mission", "zHarnessEnd.dll", "--timeout", "120", *args],
                         capture_output=True, text=True)
    summary = json.loads(out.stdout.strip().splitlines()[-1])
    assert summary["ok"], (args, summary, out.stderr)
    return summary


def jsonl(path: Path) -> list:
    return [json.loads(line) for line in path.read_text().splitlines()]


def main() -> int:
    # The whole run with state: same telemetry as the plain check (state reading changes nothing), a state.json at
    # the end, and the objective and the end in events.jsonl.
    run("--ticks", "3000", "--state", "--out", str(BUILD / "check_state"))
    st = BUILD / "check_state"
    assert (st / "telemetry.jsonl").read_bytes() == (BUILD / "check" / "telemetry.jsonl").read_bytes(), \
        "telemetry differs with --state"
    state = json.loads((st / "state.json").read_text())
    assert state["end"] == 3 and state["objectives"] == [{"met": 1, "text": "Survive until tick 800"}], state["objectives"]
    types = sorted(u["type"] for u in state["units"] if u["owner"] == 0)
    assert types == ["COMMAND", "LIGHT_TANK", "LIGHT_TANK"], types
    assert state["map"]["pad"] == 32 and len(state["cells"]) == state["map"]["width"] * state["map"]["height"]
    kinds = [e["ev"] for e in jsonl(st / "events.jsonl")]
    assert "objective" in kinds and kinds[-1] == "end", kinds

    # --stop: a Lynx sent off at tick 10 stops the run when it arrives (an idle event), long before the win.
    lynx = min(u["id"] for u in state["units"] if u["type"] == "LIGHT_TANK")
    script = BUILD / "check_stop_script.txt"
    script.write_text(f"0 coords file\n10 move {lynx} 50 60\n")
    summary = run("--ticks", "3000", "--script", str(script), "--stop", f"idle:{lynx}", "--out", str(BUILD / "check_stop"))
    stop = json.loads((BUILD / "check_stop" / "state.json").read_text())
    assert stop["stop"] == f"idle {lynx}" and 10 < stop["tick"] < 800 and summary["game_term_reason"] == "Running", (stop["stop"], stop["tick"])
    moved = next(u for u in stop["units"] if u["id"] == lynx)
    assert (moved["x"], moved["y"]) == (50, 60), moved

    # Another player's unit appearing is an `enemy` event (in the file and for --stop), never `new`.
    scn = BUILD / "check_enemy.json"
    scn.write_text(json.dumps({"map": "cm01.map", "seed": 1, "ticks": 200,
                               "spawn": [{"type": "LIGHT_TANK", "owner": 0, "tile": [20, 22], "weapon": "LASER"}]}))
    extra = BUILD / "check_enemy_script.txt"
    extra.write_text("20 spawn LIGHT_TANK 1 30 22 LASER\n")
    out = subprocess.run([str(HERE / "run.sh"), "--scenario", str(scn), "--script", str(extra), "--trace-every", "0",
                          "--stop", "enemy", "--timeout", "120", "--out", str(BUILD / "check_enemy")], capture_output=True, text=True)
    assert json.loads(out.stdout.strip().splitlines()[-1])["ok"], out.stdout + out.stderr
    events = jsonl(BUILD / "check_enemy" / "events.jsonl")
    enemy = [e for e in events if e["ev"] == "enemy"]
    assert len(enemy) == 1 and enemy[0]["owner"] == 1 and enemy[0]["tick"] == 20, events
    assert not any(e["ev"] == "new" and e["owner"] != 0 for e in events), events
    stopped = json.loads((BUILD / "check_enemy" / "state.json").read_text())
    assert stopped["stop"] == f"enemy {enemy[0]['id']}" and stopped["tick"] == 20, (stopped["stop"], stopped["tick"])

    if not INSPECT.is_file():
        print("session check: op2-inspect isn't built (cd tools && cargo build -p op2-inspect): game_* tools not checked")
        print("session check: OK")
        return 0
    env = dict(os.environ, TMPDIR=str(BUILD / "tmp"))
    (BUILD / "tmp").mkdir(exist_ok=True)

    def tool(*args: str, fail: bool = False, harness: str = "") -> str:
        e = dict(env, OP2_HARNESS=harness) if harness else env
        out = subprocess.run([str(INSPECT), *args], capture_output=True, text=True, env=e, cwd=HERE.parent)
        assert (out.returncode != 0) == fail, (args, out.stdout, out.stderr)
        return out.stdout + out.stderr

    def script() -> list:
        return [line.split("#")[0].split() for line in tool("game_observe", "what=script").splitlines()[1:]]

    text = tool("game_start", "mission=zHarnessEnd", "campaign=none", "tick=10", "name=check")
    assert "started zharnessend" in text and "Survive until tick 800" in text, text
    text = tool("game_step", f"orders=move {lynx} 50 60  # off it goes", f"until=idle:{lynx}")
    assert f"{lynx} LIGHT_TANK arrived at (50,60)" in text and f"stopped at event: idle {lynx}" in text, text
    arrived = int(text.split("ticks 10 -> ")[1].split()[0])
    assert arrived == stop["tick"], (arrived, stop["tick"])  # the same game as the plain --stop run above
    text = tool("game_observe", "what=units", "type=TANK", "near=[50,60]", "radius=1")
    assert text.startswith("1 units: LIGHT_TANK") and f"{lynx} LIGHT_TANK" in text, text
    text = tool("game_rewind", "tick=10")
    assert "dropped the 1 orders" in text, text
    tool("game_step", "orders=move 2 45 64", "ticks=100")
    assert "identical" in tool("game_verify"), "game_verify"
    # An order given at 110 for later (+300) survives a rewind to after 110, and goes with one to 110.
    tool("game_step", "orders=+300 move 3 40 60  # later; really", "ticks=50")
    assert script() == [["10", "move", "2", "45", "64"], ["410", "move", "3", "40", "60"]], script()
    assert "dropped the 0 orders" in tool("game_rewind", "tick=130")
    assert script()[-1] == ["410", "move", "3", "40", "60"], script()
    # A step that can't run (to in the past) adds none of its orders.
    tool("game_step", "orders=move 3 41 61", "to=5", fail=True)
    assert ["130", "move", "3", "41", "61"] not in script(), script()
    # A rewind whose replay fails keeps the orders and the tick.
    tool("game_rewind", "tick=50", fail=True, harness=str(BUILD / "no-harness-here"))
    assert len(script()) == 2 and tool("game_observe", "what=colony").startswith("tick 130"), script()
    # An order given now hasn't run yet: game_verify replays what the last step ran.
    tool("game_order", "orders=move 3 42 62")
    text = tool("game_verify")
    assert "identical" in text and "1 orders given since the last step" in text, text
    assert "dropped the 2 orders" in tool("game_rewind", "tick=110")
    out = BUILD / "check_session.txt"
    tool("game_save", f"out={out}")
    lines = [line for line in out.read_text().splitlines() if not line.startswith("#")]
    assert lines == ["0 coords file", "10     move 2 45 64"], lines
    assert "tool calls" in tool("game_stop")
    print("session check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
