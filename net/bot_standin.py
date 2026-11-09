#!/usr/bin/env python3
"""A scripted stand-in for an LLM playing an op2-engine bot seat (`--bot DIR`; docs/multiplayer.md "Playing against
an LLM (basic)"), for tests: it reads DIR/state.json, appends orders to DIR/orders.jsonl as bot_order does and reads
the answers in DIR/log.jsonl, deciding only from what the state shows (no unit ids known in advance).

    python3 -I net/bot_standin.py DIR [--seconds S]

What it does, each step once, in order, as soon as the state allows: one order it knows is wrong (an unknown unit, so
an error comes back); every vehicle but the ConVecs moves 3 tiles west; a ConVec with a kit builds it,
trying sites around the ConVec until one is accepted (blocked sites are answered with an error); an enabled Vehicle
Factory produces a Cargo Truck (BIG_TRUCK); an empty ConVec docks at a Structure Factory with a kit in a bay
and takes the kit (transfer); a lab researches the first topic it can. Prints each order and its answer, and at the
end a summary line `standin: N ok, M errors, steps done: ...`. Stops after S seconds (default 600) or when every step
is done and the structure count has grown (the kit was built)."""
import json
import sys
import time
from pathlib import Path


def read_state(d: Path):
    try:
        return json.loads((d / "state.json").read_text())
    except (OSError, ValueError):
        return None


class Seat:
    def __init__(self, d: Path):
        self.d = d
        self.n = 0
        self.ok = 0
        self.errors = 0

    def order(self, o: dict, wait=10.0):
        """Appends one order and waits for its answer (None if none came)."""
        self.n += 1
        o = dict(o, id=f"s{self.n}")
        with open(self.d / "orders.jsonl", "a") as f:
            f.write(json.dumps(o) + "\n")
        end = time.monotonic() + wait
        while time.monotonic() < end:
            try:
                for line in (self.d / "log.jsonl").read_text().splitlines():
                    a = json.loads(line)
                    if a.get("id") == o["id"]:
                        if a.get("ok"):
                            self.ok += 1
                        else:
                            self.errors += 1
                        print(f"standin: {json.dumps(o)} -> {json.dumps(a)}", flush=True)
                        return a
            except (OSError, ValueError):
                pass
            time.sleep(0.1)
        print(f"standin: {json.dumps(o)} -> no answer", flush=True)
        return None


def spiral(x, y, radius):
    out = []
    for r in range(2, radius + 1):
        for dx in range(-r, r + 1):
            for dy in range(-r, r + 1):
                if max(abs(dx), abs(dy)) == r:
                    out.append((x + dx, y + dy))
    return out


def main(argv) -> int:
    if not argv or argv[0].startswith("-"):
        print(__doc__)
        return 2
    d = Path(argv[0])
    seconds = float(argv[argv.index("--seconds") + 1]) if "--seconds" in argv else 600.0
    seat = Seat(d)
    done = []
    docker = None
    first_structures = None
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        s = read_state(d)
        if not s:
            time.sleep(0.5)
            continue
        units, structures = s["units"], s["structures"]
        if first_structures is None:
            first_structures = len(structures)
            print(f"standin: first state at tick {s['tick']}: {len(units)} units, {len(structures)} structures, "
                  f"{len(s['others'])} others in sight", flush=True)
        if "error" not in done:
            seat.order({"do": "move", "unit": 999999, "tile": [10, 10]})
            done.append("error")
        if "move" not in done:
            movers = [u for u in units if u["type"] != "CON_TRUCK"]
            if movers:
                for u in movers:
                    seat.order({"do": "move", "unit": u["id"], "tile": [u["tile"][0] - 3, u["tile"][1]]})
                done.append("move")
        # A kit from a Structure Factory's bay: dock an empty ConVec there, then transfer the bay once it is on the dock.
        sf = next((b for b in structures if b["type"] == "FACT_STRUCTURE" and any(b.get("bays") or [])), None)
        if "dock" not in done and sf and not any(u.get("kit") for u in units):
            convec = next((u for u in units if u["type"] == "CON_TRUCK" and not u.get("kit")), None)
            if convec:
                a = seat.order({"do": "dock", "unit": convec["id"], "at": sf["id"]})
                if a and a.get("ok"):
                    done.append("dock")
                    docker = convec["id"]
        if "dock" in done and "transfer" not in done and sf:
            u = next((u for u in units if u["id"] == docker), None)
            if u and u["tile"] == sf.get("dock") and u.get("idle"):
                bay = next(i for i, b in enumerate(sf["bays"]) if b)
                seat.order({"do": "transfer", "unit": sf["id"], "bay": bay})
                done.append("transfer")
        if "build" not in done:
            convec = next((u for u in units if u.get("kit") and u.get("idle")), None)
            if convec:
                for x, y in spiral(*convec["tile"], 8):
                    a = seat.order({"do": "build", "unit": convec["id"], "tile": [x, y]})
                    if a and a.get("ok"):
                        done.append("build")
                        break
                else:
                    done.append("build (no site)")
        if "produce" not in done:
            vf = next((b for b in structures if b["type"] == "FACT_VEHICLE" and b.get("enabled") and b.get("cmd") != "Produce"), None)
            if vf:
                seat.order({"do": "produce", "unit": vf["id"], "type": "BIG_TRUCK"})
                done.append("produce")
        if "research" not in done:
            labs = {"LAB": 1, "LAB_STANDARD": 2, "LAB_ADV": 3}
            lab = next((b for b in structures if b["type"] in labs and b.get("enabled") and b.get("cmd") != "Research"), None)
            topic = lab and next((t for t in s["research"]["available"] if t.get("lab") == labs[lab["type"]]), None)
            if topic:
                seat.order({"do": "research", "unit": lab["id"], "topic": topic["id"]})
                done.append("research")
        if "build" in done and len(structures) > first_structures and all(k in done for k in ("error", "move", "produce", "research")):
            print(f"standin: tick {s['tick']}: {len(structures)} structures (was {first_structures})", flush=True)
            break
        time.sleep(1.0)
    s = read_state(d) or {}
    print(f"standin: {seat.ok} ok, {seat.errors} errors, steps done: {', '.join(done) or 'none'}; last state tick {s.get('tick')}, "
          f"{len(s.get('structures', []))} structures, {len(s.get('units', []))} units", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
