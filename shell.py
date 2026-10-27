#!/usr/bin/env python3
"""Boot the original game normally (intro video, title, shell menus, briefings) under Xvfb in a Wine virtual desktop,
drive it with real X input from a driver script, take screenshots of the whole desktop, and log what the game does.

    harness/shell.sh --drive harness/oracle/shell/eden1.txt --out /tmp/shell

The harness module runs in shell mode (OP2H_SHELL=1, see harness.cpp "Shell mode"): it only watches, writing
shell.jsonl (movies, the shell's state messages, dialogs and their controls, files, game start and end). Outputs in
--out: shell.jsonl, driver.jsonl (every driver step with its time), shot_<name>.png (the whole desktop), summary.json
(the movies with their timings, the dialogs, the screenshots), harness.log, wine.log, outpost2.ini, and with --audio
audio.jsonl for the in-game part.

Driver script, one step per line ('#' starts a comment). Times are wall-clock seconds:
    wait <ev> [field=value | field=~regex ...] [timeout=S]   next shell.jsonl event of kind <ev> whose fields match
                                                              (searched from just after the previous wait's match)
    sleep <S>                                                 wait S seconds
    shot <name>                                               screenshot -> shot_<name>.png
    shots <name> <count> <interval S>                         a series: shot_<name>_00.png ...
    click <x> <y> [right]                                     mouse click at desktop pixel x, y
    ctl <text | id=N> [in=<window text regex>] [right]        click the centre of a control (a `dump` finds it; '_' in
                                                              text stands for a space)
    move <x> <y>                                              move the pointer
    key <keysym> [keysym ...]                                 key presses (X keysym names: Escape Return space a F1 ...)
    dump [tag]                                                log every window and control (shell.jsonl `dump`)
    state [tag] [after=N]                                     log the game's tick, pause flag, cursor handle, selection
                                                              (with the units' commands) and view (shell.jsonl `state`;
                                                              flow.json `probe` without the tick and cursor). Running:
                                                              read by the game thread at the end of the cycle N ticks
                                                              after the request, so it counts ticks, not wall time;
                                                              paused: at once (no cycle runs, nothing moves)
    mark <text>                                               a marker in driver.jsonl
    quit                                                      end the run (also the end of the script)
"""
import argparse
import ctypes
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run  # noqa: E402  (shares the prefix, install check and Xvfb helpers)

class X:
    """Real input through the XTEST extension of the run's X server."""

    def __init__(self, display: str):
        self.x11 = ctypes.CDLL("libX11.so.6")
        self.xtst = ctypes.CDLL("libXtst.so.6")
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.x11.XStringToKeysym.restype = ctypes.c_ulong
        self.x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
        self.x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.dpy = self.x11.XOpenDisplay(display.encode())
        if not self.dpy:
            sys.exit(f"cannot open display {display}")

    def move(self, x: int, y: int):
        self.xtst.XTestFakeMotionEvent(self.dpy, -1, x, y, 0)
        self.x11.XFlush(self.dpy)

    def click(self, x: int, y: int, button: int = 1):
        self.move(x, y)
        time.sleep(0.05)
        self.xtst.XTestFakeButtonEvent(self.dpy, button, 1, 0)
        self.x11.XFlush(self.dpy)
        time.sleep(0.08)
        self.xtst.XTestFakeButtonEvent(self.dpy, button, 0, 0)
        self.x11.XFlush(self.dpy)

    def key(self, name: str):
        mods, _, base = name.rpartition("+")
        codes = []
        for k in [*(m for m in mods.split("+") if m), base]:
            k = {"ctrl": "Control_L", "shift": "Shift_L", "alt": "Alt_L"}.get(k.lower(), k)
            sym = self.x11.XStringToKeysym(k.encode())
            if not sym:
                raise ValueError(f"unknown keysym {k}")
            codes.append(self.x11.XKeysymToKeycode(self.dpy, sym))
        for c in codes:
            self.xtst.XTestFakeKeyEvent(self.dpy, c, 1, 0)
            self.x11.XFlush(self.dpy)
            time.sleep(0.03)
        time.sleep(0.05)
        for c in reversed(codes):
            self.xtst.XTestFakeKeyEvent(self.dpy, c, 0, 0)
            self.x11.XFlush(self.dpy)
            time.sleep(0.03)


class Events:
    """Tails shell.jsonl."""

    def __init__(self, path: Path):
        self.path = path
        self.events: list[dict] = []
        self.pos = 0  # Byte offset read so far.
        self.cursor = 0  # Index after the last `wait` match.
        self.t_init: float | None = None  # time.monotonic() at the harness's ms 0 (to within a poll interval).

    def poll(self):
        if not self.path.exists():
            return
        with open(self.path, "rb") as f:
            f.seek(self.pos)
            data = f.read()
        end = data.rfind(b"\n")
        if end < 0:
            return
        for line in data[:end].splitlines():
            try:
                e = json.loads(line.decode("latin-1"))
            except json.JSONDecodeError:
                e = {"ev": "bad", "line": line[:200].decode("latin-1")}
            if e.get("ev") == "init" and self.t_init is None:
                self.t_init = time.monotonic() - e["ms"] / 1000
            self.events.append(e)
        self.pos += end + 1

    @staticmethod
    def matches(e: dict, ev: str, conds: list[tuple[str, str]]) -> bool:
        if e.get("ev") != ev:
            return False
        for k, v in conds:
            have = e.get(k)
            if have is None:
                return False
            have = have if isinstance(have, str) else json.dumps(have)
            if v.startswith("~"):
                if not re.search(v[1:], have, re.I):
                    return False
            elif have != v:
                return False
        return True

    def wait(self, ev: str, conds, timeout: float, alive) -> dict | None:
        deadline = time.monotonic() + timeout
        while True:
            self.poll()
            for i in range(self.cursor, len(self.events)):
                if self.matches(self.events[i], ev, conds):
                    self.cursor = i + 1
                    return self.events[i]
            if time.monotonic() > deadline or not alive():
                return None
            time.sleep(0.02)


def grab(display: str, path: Path):
    from PIL import ImageGrab
    ImageGrab.grab(xdisplay=display).save(path)


def find_control(dump: dict, what: str, within: str | None):
    """The control whose text contains `what` (case-insensitive, '&' ignored), or whose id is `id=N`, in a visible
    window. The last match wins (dialogs are listed topmost first, controls in tab order)."""
    best = None
    for w in dump.get("windows", []):
        if not w.get("visible"):
            continue
        for c in [w, *w.get("children", [])]:
            if not c.get("visible"):
                continue
            if within and not re.search(within, w.get("text", ""), re.I) and not re.search(within, c.get("text", ""), re.I):
                continue
            if what.startswith("id="):
                hit = str(c.get("id")) == what[3:] and c is not w
            else:
                hit = what.lower() in c.get("text", "").replace("&", "").lower() and c is not w
            if hit:
                best = c
    return best


def crop_shots(out: Path, events: list[dict]) -> list | None:
    """Crops every screenshot to the game's window (the shell and the game frame share one 640x480 rectangle, centred
    on the desktop); returns that rectangle in desktop pixels."""
    rect = next((e["rect"] for e in events if e.get("ev") == "show" and e.get("cls") == "Outpost2Shell"), None)
    if not rect:
        return None
    from PIL import Image
    x, y, w, h = rect
    for f in sorted(out.glob("shot_*.png")):
        im = Image.open(f)
        if im.size != (w, h):
            im.crop((x, y, x + w, y + h)).save(f)
    return rect


def flow(events: list[dict], window: list | None) -> list[dict]:
    """The deterministic part of shell.jsonl: what the game showed and did, in order, without times, handles or the
    random seed. Dialog layouts come from the driver's dumps, in the game window's coordinates."""
    ox, oy = (window or [0, 0])[:2]
    out: list[dict] = []
    movie = {}

    def rel(r):
        return [r[0] - ox, r[1] - oy, r[2], r[3]]

    for e in events:
        ev = e.get("ev")
        if ev == "modal":
            out.append({"modal": e["res"], "module": e.get("module")})
        elif ev == "modeless":
            out.append({"modeless": e["res"], "module": e.get("module")})
        elif ev == "modal_end":
            out.append({"modal_end": e["res"], "ret": e.get("ret")})
        elif ev == "dialog":
            out.append({"dialog": e.get("text"), "controls": [[c["id"], c["cls"], c["text"]] for c in e.get("controls", [])]})
        elif ev == "post" and e.get("msg") != "0x732":
            out.append({"state": e["msg"]})
        elif ev == "file":
            entry = {"file": e["name"], "found": e.get("found")}
            if not out or out[-1] != entry:
                out.append(entry)
        elif ev == "mci" and e.get("cmd") == "open" and e.get("ret") == 0:
            movie = {"movie": (e.get("file") or "").split("\\")[-1], "type": e.get("type")}
            out.append(movie)
        elif ev == "mci" and e.get("cmd") == "where":
            movie["rect"] = e.get("rect")
        elif ev == "notify" and movie:
            movie.setdefault("notify", e.get("flags"))
        elif ev == "mci" and e.get("cmd") == "close" and movie:
            movie.setdefault("notify", None)
            movie["ended"] = "to the end" if movie.get("notify") == 1 and "skipped" not in movie else "skipped"
            movie = {}
        elif ev == "input" and movie and e.get("cls") == "AviWnd":
            movie["skipped"] = e.get("msg")
        elif ev == "start":
            out.append({"start": {k: v for k, v in e.items() if k not in ("ms", "ev", "seed")}})
        elif ev == "state":  # Without the tick, which depends on real time.
            out.append({"probe": e.get("tag"), **{k: e.get(k) for k in ("paused", "selected", "cmds", "view")}})
        elif ev == "game" and e.get("what") == "end":
            out.append({"game_end": e.get("reason"), "tick": e.get("tick")})
        elif ev == "activate_shell":
            out.append({"activate_shell": e.get("reason"), "mission": e.get("mission")})
        elif ev == "dump":
            wins = []
            for w in e.get("windows", []):
                if not w.get("visible") or w.get("cls") in ("IME", "Wine IME"):
                    continue
                kids = [[c["id"], c["cls"], c["text"], rel(c["rect"])] for c in w.get("children", []) if c.get("visible")]
                wins.append({"cls": w["cls"], "text": w["text"], "rect": rel(w["rect"]), "controls": kids})
            if not str(e.get("tag")).startswith("ctl"):  # The `ctl` steps' own lookups.
                out.append({"layout": e.get("tag"), "windows": wins})
    return out


def summarize(out: Path, events: list[dict], driver: list[dict]) -> dict:
    """Movies (open .. close, with how they ended), dialogs and modal loops, files, game start/end."""
    movies, open_by_dev = [], {}
    for e in events:
        if e.get("ev") == "mci" and e.get("cmd") == "open" and e.get("ret") == 0:
            m = {"file": e.get("file"), "type": e.get("type"), "dev": e.get("newdev"), "open_ms": e["ms"]}
            open_by_dev[e.get("newdev")] = m
            movies.append(m)
        elif e.get("ev") == "mci" and e.get("dev") in open_by_dev:
            m = open_by_dev[e["dev"]]
            if e.get("cmd") == "play":
                m["play_ms"] = e["ms"]
            elif e.get("cmd") in ("stop", "close") and "end_ms" not in m:
                m["end_ms"] = e["ms"]
                m["ended_by"] = m.get("ended_by", e["cmd"])
            elif e.get("cmd") in ("put", "where", "window"):
                m.setdefault(e["cmd"], e.get("rect") or e.get("window"))
        elif e.get("ev") == "notify" and e.get("dev") in open_by_dev:
            m = open_by_dev[e["dev"]]
            m.setdefault("notify", []).append({"ms": e["ms"], "flags": e.get("flags")})
    for m in movies:
        if "play_ms" in m:
            # A skipped movie is closed first; its "successful" notification arrives after the close.
            ends = [n["ms"] for n in m.get("notify", []) if n["flags"] == 1 and n["ms"] <= m.get("end_ms", n["ms"])]
            m["played_s"] = round(((ends[0] if ends else m.get("end_ms", m["play_ms"])) - m["play_ms"]) / 1000, 2)
            m["to_end"] = bool(ends)
    keep = ("modal", "modal_end", "modeless", "dialog", "start", "game", "lib")
    flow = [{k: v for k, v in e.items() if k not in ("controls",)} for e in events if e.get("ev") in keep]
    posts = [{"ms": e["ms"], "msg": e["msg"], "w": e.get("w"), "l": e.get("l")} for e in events if e.get("ev") in ("post", "send")]
    files = sorted({e["name"] for e in events if e.get("ev") == "file"})
    return {"movies": movies, "flow": flow, "posts": posts, "files": files,
            "shots": [d for d in driver if d.get("step") in ("shot", "shots")]}


class SavesAside:
    """Moves the game's saved games out of the way for a run and back afterwards (the SAVE and LOAD dialogs list
    them, so a run that saves needs the same empty folder every time). The run's own saves are kept in
    <out>/saves."""

    def __init__(self, saves: Path, out: Path):
        import fcntl
        # Runs with --clean-saves take turns (two at once moved each other's folders: `record --jobs 2`).
        self.lock = open(saves.parent / "saves.harness-lock", "w")
        fcntl.flock(self.lock, fcntl.LOCK_EX)
        self.saves, self.out = saves, out
        self.aside = saves.parent / f"saves.harness-aside-{os.getpid()}"
        saves.mkdir(parents=True, exist_ok=True)
        saves.rename(self.aside)
        saves.mkdir()

    def restore(self):
        kept = self.out / "saves"
        shutil.rmtree(kept, ignore_errors=True)
        shutil.move(self.saves, kept)
        self.aside.rename(self.saves)
        self.lock.close()


class WindowSize:
    """Sets the window's size for a run. The game frame (0x49B463) takes [GAME_WINDOW] H_SIZE x V_SIZE at H_POS, V_POS
    from outpost2.ini when the saved [DESKTOP] size fits in the current desktop, the size is at least 640x480 and the
    position is on the desktop, else 640x480 centred (maximized when that is the whole desktop); OP2Shell then makes
    its window that size, or the whole desktop with MAXIMIZED=1 (0x13007DC0). The harness DLL copies prefix-harness's
    OPU/outpost2.ini to the run's private one at start-up, so the keys go into that source for the run (the window
    at the desktop's top left, [DESKTOP] the run's desktop) and the file is put back byte for byte afterwards. Runs
    with --window take turns (a lock beside the ini, held until the file is back); a parallel run without it may still
    start with this run's window."""

    def __init__(self, ini: Path, spec: str, desktop: tuple[int, int]):
        import fcntl
        self.lock = open(ini.with_name(ini.name + ".harness-lock"), "w")
        fcntl.flock(self.lock, fcntl.LOCK_EX)
        self.ini, self.saved = ini, ini.read_bytes()
        window = {"MAXIMIZED": "1"} if spec == "max" else dict(
            zip(("MAXIMIZED", "H_POS", "V_POS", "H_SIZE", "V_SIZE"), ("0", "0", "0", *spec.lower().split("x"))))
        text = self.saved.decode("latin-1")
        for section, keys in (("GAME_WINDOW", window), ("DESKTOP", {"H_SIZE": str(desktop[0]), "V_SIZE": str(desktop[1])})):
            sec = re.search(rf"^\[{section}\]\r?\n((?:[^\[\r\n].*\r?\n)*)", text, re.M)
            if not sec:
                text += f"[{section}]\r\n"
                sec = re.search(rf"^\[{section}\]\r?\n()", text, re.M)
            body = sec.group(1)
            for k, v in keys.items():
                body, n = re.subn(rf"^{k}=[^\r\n]*", f"{k}={v}", body, flags=re.M)
                if not n:
                    body += f"{k}={v}\r\n"
            text = text[:sec.start(1)] + body + text[sec.end(1):]
        ini.write_bytes(text.encode("latin-1"))

    def restore(self):
        self.ini.write_bytes(self.saved)
        self.lock.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--drive", type=Path, required=True, help="driver script (see above)")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--desktop", default="800x600", help="Wine virtual desktop (and X screen) size")
    ap.add_argument("--script", type=Path, help="in-game tick script (harness verbs, e.g. a mission's winning orders)")
    ap.add_argument("--audio", action="store_true", help="in-game audio.jsonl (README \"Audio log\")")
    ap.add_argument("--telemetry-every", type=int, default=0, metavar="N", help="in-game telemetry every N ticks")
    ap.add_argument("--ini", action="append", default=[], metavar="KEY=VALUE",
                    help="[Game] setting in the run's private outpost2.ini, e.g. IntroAVI=1 (repeatable)")
    ap.add_argument("--timeout", type=float, default=600, help="wall-clock seconds before the game is killed")
    ap.add_argument("--display", help="use this X display instead of a private Xvfb (e.g. to watch)")
    ap.add_argument("--clean-saves", action="store_true",
                    help="run with an empty OPU/saves (the saved games SGAME*.OP2), putting the old ones back after")
    ap.add_argument("--window", metavar="WxH|max",
                    help="the shell window's size ([GAME_WINDOW] H_SIZE/V_SIZE; max: MAXIMIZED=1, the desktop's size)")
    args = ap.parse_args()
    if args.window and args.window != "max" and not re.fullmatch(r"\d+x\d+", args.window.lower()):
        ap.error("--window takes WxH or max")

    run.sync_missions_from_play_prefix()
    run.check_installed()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for old in [*out.glob("shot_*.png"), *(out / n for n in ("shell.jsonl", "driver.jsonl", "summary.json", "harness.log",
                                                              "audio.jsonl", "telemetry.jsonl", "ctl.txt"))]:
        old.unlink(missing_ok=True)
    w, h = (int(v) for v in args.desktop.lower().split("x"))
    if args.display:
        xvfb, display = None, args.display
    else:
        r, wfd = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(wfd), "-screen", "0", f"{w}x{h}x24", "-nolisten", "tcp"],
                                pass_fds=(wfd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(wfd)
        with os.fdopen(r) as f:
            display = ":" + f.readline().strip()
    env = dict(os.environ, DISPLAY=display, WINEPREFIX=str(run.PREFIX), WINEDEBUG="-all",
               WINEDLLOVERRIDES="winepulse.drv=d;winealsa.drv=d;winedbg.exe=d",
               OP2H_SHELL="1", OP2H_OUT=run.win_path(out), OP2H_AUDIO="1" if args.audio else "0",
               OP2H_TELEM_EVERY=str(args.telemetry_every))
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("OP2H_MISSION", None)
    for kv in args.ini:
        k, _, v = kv.partition("=")
        env[f"OP2H_INI_{k}"] = v
    if args.script:
        env["OP2H_SCRIPT"] = run.win_path(args.script.resolve())
    # The program needs its full Windows path: `explorer /desktop=... Outpost2.exe` silently starts nothing.
    exe = "C:\\" + str((run.GAME_DIR / "Outpost2.exe").relative_to(run.PREFIX / "drive_c")).replace("/", "\\")

    saves = SavesAside(run.GAME_DIR / "OPU" / "saves", out) if args.clean_saves else None
    window_size = WindowSize(run.GAME_DIR / "OPU" / "outpost2.ini", args.window, (w, h)) if args.window else None
    events = Events(out / "shell.jsonl")
    driver: list[dict] = []
    log = open(out / "wine.log", "w")
    t0 = time.monotonic()
    # One desktop per run: parallel runs on other X displays must not join this one.
    game = subprocess.Popen(["wine", "explorer", f"/desktop=op2h{os.getpid()},{w}x{h}", exe, "/OPU"], cwd=run.GAME_DIR, env=env,
                            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    alive = lambda: game.poll() is None and time.monotonic() - t0 < args.timeout  # noqa: E731
    x = X(display)
    x.move(w - 1, h - 1)

    def record(step: str, **kw):
        events.poll()
        d = {"t": round(time.monotonic() - t0, 3), "step": step, **kw}
        if events.t_init is not None:
            d["ms"] = round((time.monotonic() - events.t_init) * 1000, 1)  # On shell.jsonl's clock.
        driver.append(d)
        print(json.dumps(d), flush=True)

    def dump(tag: str) -> dict | None:
        tmp = out / "ctl.tmp"
        tmp.write_text(f"dump {tag}\n")
        tmp.rename(out / "ctl.txt")
        return events.wait("dump", [("tag", tag)], 10, alive)

    def state(tag: str, after: int) -> dict | None:
        tmp = out / "ctl.tmp"
        tmp.write_text(f"state {tag} {after}\n")
        tmp.rename(out / "ctl.txt")
        return events.wait("state", [("tag", tag)], 10 + after / 10, alive)

    def steps() -> tuple[bool, str | None]:
        n_dump = 0
        for lineno, raw in enumerate(args.drive.read_text().splitlines(), 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            verb, *a = line.split()
            if not alive():
                return False, f"line {lineno}: game not running ({line})"
            if verb == "wait":
                timeout = 60.0
                conds = []
                for c in a[1:]:
                    k, _, v = c.partition("=")
                    if k == "timeout":
                        timeout = float(v)
                    else:
                        conds.append((k, v))
                e = events.wait(a[0], conds, timeout, alive)
                if e is None:
                    record("wait", what=line, matched=None)
                    return False, f"line {lineno}: timed out: {line}"
                record("wait", what=line, matched_ms=e["ms"])
            elif verb == "sleep":
                time.sleep(float(a[0]))
            elif verb == "shot":
                grab(display, out / f"shot_{a[0]}.png")
                record("shot", name=a[0])
            elif verb == "shots":
                name, count, every = a[0], int(a[1]), float(a[2])
                for i in range(count):
                    grab(display, out / f"shot_{name}_{i:02d}.png")
                    record("shots", name=f"{name}_{i:02d}")
                    time.sleep(every)
            elif verb == "click":
                x.click(int(a[0]), int(a[1]), 3 if "right" in a[2:] else 1)
                record("click", at=[int(a[0]), int(a[1])])
            elif verb == "ctl":
                within = next((v[3:] for v in a[1:] if v.startswith("in=")), None)
                n_dump += 1
                d = dump(f"ctl{n_dump}")
                c = find_control(d, a[0].replace("_", " "), within) if d else None
                if not c:
                    record("ctl", what=a[0], found=None)
                    return False, f"line {lineno}: no control {a[0]!r}"
                cx, cy = c["rect"][0] + c["rect"][2] // 2, c["rect"][1] + c["rect"][3] // 2
                x.click(cx, cy, 3 if "right" in a[1:] else 1)
                record("ctl", what=a[0], id=c.get("id"), text=c.get("text"), at=[cx, cy])
            elif verb == "move":
                x.move(int(a[0]), int(a[1]))
                record("move", at=[int(a[0]), int(a[1])])
            elif verb == "key":
                for k in a:
                    x.key(k)
                    record("key", key=k)
            elif verb == "dump":
                dump(a[0] if a else "dump")
                record("dump", tag=a[0] if a else "dump")
            elif verb == "state":
                tag = next((v for v in a if not v.startswith("after=")), "state")
                after = int(next((v[6:] for v in a if v.startswith("after=")), "0"))
                e = state(tag, after)
                if e is None:
                    record("state", tag=tag, found=None)
                    return False, f"line {lineno}: no answer: {line}"
                record("state", tag=tag, **{k: e.get(k) for k in ("tick", "paused", "pauseCount", "cursor", "selected", "cmds", "view")})
            elif verb == "mark":
                record("mark", text=" ".join(a))
            elif verb == "quit":
                break
            else:
                return False, f"line {lineno}: unknown step {verb}"
        return True, None

    try:
        ok, failed = steps()
    except Exception as e:  # Still clean up the game, the wineserver and Xvfb.
        ok, failed = False, f"driver error: {e!r}"
    record("end", ok=ok, failed=failed)
    if game.poll() is None:
        (out / "ctl.txt").write_text("quit\n")
        try:
            game.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
    if game.poll() is None:
        os.killpg(game.pid, signal.SIGKILL)
    # The virtual desktop's explorer.exe exits with the game. The wineserver is shared with any parallel harness runs.
    if xvfb:
        xvfb.terminate()
        xvfb.wait()
    if saves:
        saves.restore()
    if window_size:
        window_size.restore()
    events.poll()
    (out / "ctl.txt").unlink(missing_ok=True)
    with open(out / "driver.jsonl", "w") as f:
        for d in driver:
            f.write(json.dumps(d) + "\n")
    window = crop_shots(out, events.events)
    summary = {"ok": ok, "failed": failed, "wall_seconds": round(time.monotonic() - t0, 1), "window": window,
               **summarize(out, events.events, driver)}
    (out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    (out / "flow.json").write_text(json.dumps(flow(events.events, window), indent=1) + "\n")
    print(json.dumps({"ok": ok, "failed": failed, "movies": [(m["file"], m.get("played_s")) for m in summary["movies"]]}))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
