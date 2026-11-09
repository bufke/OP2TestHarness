# Architecture

How OP2TestHarness is put together, for contributors and agents new to the code. It follows the template at
architecture.md. Keep it current when modules or data flow change.

OP2TestHarness turns the original Outpost 2 into a measuring instrument. A DLL loaded into the real game plays a
scenario or mission unattended, with a fixed seed and scripted orders, and writes what happened (unit state every
tick, telemetry, screenshots) as files. A reimplementation then plays the same recipe and the two are compared: op2-rs, which it was written for, compares
its simulation field by field each tick and its engine's pictures with the original's; outpost2-port, a C++/SDL2
port, plays the recipe through its own `--harness` mode and writes the same trace format, compared here by
`trace_diff.py`. It started as op2-rs's `harness/` (PROVENANCE.md) and adds a native Windows build, launcher and the
comparison tools in `windows/`.

## 1. Project structure

```
OP2TestHarness/
├── src/
│   ├── harness.cpp        the op2ext module (harness.dll): everything that runs inside the game, ~5,000 lines
│   └── mingw_compat.h     force-included under MinGW: the MSVC-isms TethysAPI expects (min/max, <cstring>)
├── test/
│   ├── Scenario.cpp       zScenario.dll: the near-empty mission every scenario runs in (map name patched per run)
│   ├── EndTest.cpp        zHarnessEnd.dll: self-test mission, won at tick 800
│   └── *_check.py         end-to-end checks: session state, checkpoints, play-and-record (Linux)
├── bots/*.cpp             optional per-tick plug-in DLLs (HarnessBotTick): hit masks, line of sight, cursor, edges
├── windows/               native Windows (no Wine; not in op2-rs's copy)
│   ├── build.bat          MSVC x86 build of harness.dll and zScenario.dll into build\win
│   ├── msvc_compat.h      force-included under MSVC: C++20, NOMINMAX, _ReturnAddress, #undef small
│   ├── run_win.py         `setup`: private game copy with the harness installed; `run`: play a scenario
│   ├── compare.py         original vs op2-rs on one scenario: sim compare, both pictures side by side
│   ├── run_port.py        `install`: outpost2-port's build into the game copy; `run`: play a scenario in the port
│   ├── trace_diff.py      any two traces field by field: MATCH, or the first tick each field differs
│   ├── compare_port.py    original vs outpost2-port (and op2-rs) on one or more scenarios, with a scoreboard
│   └── README.md          Windows usage, what's verified, known differences
├── run.py, run.sh         Linux launcher: a mission or scenario under Wine + Xvfb (also the scenario translator)
├── shell.py, shell.sh     Linux: boot the game normally and drive its menus with X input (shell mode)
├── netgame.py             Linux: 2-6 game copies in a LAN game, each logged
├── prefixes.py            which Wine prefix runs use; refuses the play prefix
├── tools/
│   ├── ini.py             registers/unregisters the module in OPU/outpost2.ini (with a one-time backup)
│   └── packetize.py       script order verbs -> raw `packet` lines (the UI's command packets)
├── net/                   multiplayer test scripts and a scripted bot seat for op2-rs's engine
├── Makefile               Linux/MinGW build, install into the Wine prefix, self-test
├── README.md              the full manual: options, script verbs, outputs, recording, checkpoints, gotchas
├── PROVENANCE.md          where it came from; what still follows op2-rs's layout
├── LICENSE                GPL-3.0 (op2-rs's)
├── VERSION, CHANGELOG.md  the version and what changed; the contract and how changes to it move the version
│
│   ignored (.gitignore), never committed
├── game/                  the private copy of the game run_win.py runs (game files)
├── build/                 built DLLs and run outputs (gamedata.json is game data)
└── vendor/include/Tethys  TethysAPI at 141fd47 (an upstream clone)
```

## 2. High-level system diagram

```
 scenario JSON ($ORACLE/scenarios)           ┌───────────────── the original game process ─────────────────┐
   map, seed, ticks, spawns, orders          │  Outpost2.exe /OPU                                          │
            │                                │    └─ OPU op2ext loads [ExternalModules] Harness=yes        │
            ▼                                │         └─ harness.dll InitMod: reads OP2H_* env vars,      │
 launcher (run_win.py / run.py)              │            patches the exe in memory                        │
   scenario -> harness script ─OP2H_SCRIPT──►│  StartMissionHook ─► PrepareGame(zs_<map>.dll, seed)        │
   zScenario.dll -> zs_<map>.dll             │  GameCycleHook, every tick:                                 │
   OP2H_* environment ──────────────────────►│     real ProcessGameCycle, then script verbs, bot,          │
   starts the game, timeout, cleanup         │     trace / telemetry / screenshot, quit at the tick limit  │
            │                                └──────────────────────────┬──────────────────────────────────┘
            │                                                           │ files in OP2H_OUT
            │                                                           ▼
            │                       trace.jsonl  telemetry.jsonl  gamedata.json  shot_*.bmp  harness.log
            │                                    │                      │             │
            ▼                                    ▼                      ▼             ▼
   summary.json, PNGs          op2-inspect compare (op2-rs): op2-sim replays     op2-engine (op2-rs) draws
                               the scenario with gamedata.json's rules and       the same tick; compare.py
                               diffs every field each tick -> MATCH / first      puts both side by side
                               divergence, trajectory plot
```

The boundary is the process: only `harness.cpp` runs inside the game. Launchers talk to it one way, through
environment variables and the script file, and read back only the files it writes (in shell mode also a `ctl.txt`
command file it polls).

## 3. Core components

### harness.dll (src/harness.cpp)
A 32-bit Windows DLL, the op2ext module. With no `OP2H_MISSION` or `OP2H_SHELL` in the environment, `InitMod` returns
at once and the game plays normally. `Outpost2.exe` always loads at 0x400000, so the patches are at fixed addresses
(each cited in the source); `RelinkCall` redirects a CALL and keeps the old target, `HookEntry` checks the bytes
before patching a function's start.

The file reads top to bottom by area:

| Area | What it does |
|---|---|
| Config, `Env` | `OP2H_*` variables into `cfg` |
| Code patching | `WriteCode`, `RelinkCall`, `JumpTo`; turbo (frame limiter 0x49C374), pane drawing on/off (0x49C5FC) |
| Screenshots | the game window's client area to a 24-bit BMP (GDI BitBlt) |
| Telemetry, trace | `WriteTelemetry` (periodic state), `WriteTrace` (each unit's sub-pixel position, heading, HP, order, target, RNG) |
| `WriteGameData` | gamedata.json at the first tick: map geometry, terrain speeds, unit stats as loaded (sheets + OPU patches) |
| Script | `LoadScript`, `RunScriptCmd`: `<tick> <verb> <args>` lines (spawn, setcell, move, attack, research, view, shot...) |
| Virtual input | IAT-patched `GetCursorPos`/`GetAsyncKeyState` plus posted messages: clicks and keys without the real mouse |
| Names, groups, economy | unit naming (`@N`, `$name`), group verbs, economy verbs |
| Audio log | hooks on the sound, Savant, message and music calls (`--audio`) |
| Session state | events.jsonl, state.json, `--stop` conditions |
| Recording | the local player's command packets at 0x489FF5; save/load hooks with sidecar folders; `packet` replay |
| Checkpoints | save at the end of a run, resume from it; the mission DLL's writable state saved and checked |
| Network | packet and socket hooks for LAN games (shell mode) |
| `GameCycleHook` | the per-tick driver: original cycle, script, bot, captures, stop/quit |
| `StartMissionHook` | replaces the no-shell debug map with the requested mission: GameStartInfo, campaign carry-over, multiplayer |
| Shell mode | normal boot, only watched: dialogs, windows, MCI movies, shell messages to shell.jsonl; `ctl.txt` commands |
| `InitMod`, `RunMod` | entry points: pick the mode, install the hooks; `RunMod` (shell loaded) ends a mission run |

A run ends in `Quit`: final captures, files closed, `TerminateProcess`, skipping teardown that could block on dialogs
or write the ini.

### Scenario mission (test/Scenario.cpp)
An empty mission with a 64-byte `MapName` slot after a marker. The launcher writes the map name into a copy,
`OPU/Harness/zs_<map>.dll`, because the game reads `MapName` from the DLL file. The harness script does the rest.

### Launchers
- **windows/build.bat**: the release's ten DLLs with MSVC (x86, `/MT`, `/Brepro`: reproducible), the Makefile's `all`.
- **windows/package.py**: the release zip (repository layout, DLLs in `build/win`, `SHA256SUMS`), reproducible.
- **windows/run_win.py**: `setup` copies the game (robocopy, no movies or OPU backups) to `game/`, installs the DLLs
  and registers the module there. `run` builds the script with run.py's `scenario_script`, sets `OP2H_*`, starts
  `Outpost2.exe /OPU`, kills it on timeout (`taskkill /T`), converts BMPs through System.Drawing, writes summary.json.
- **windows/compare.py**: run_win.py `run`, then `op2-inspect compare`, then `op2-engine --scenario --until-tick
  --screenshot` at 1024x768, then the side-by-side PNG (`side_by_side` takes any number of panels).
- **windows/run_port.py**: the same scenario script, played by `outpost2_modern.exe --harness` in the game copy (the
  port reads the game's files from its working directory). The port's harness mode is the port's own code
  (`src/harness_mode.cpp` there): it starts the scenario mission built in (the zScenario.dll can't run in the port: it
  calls Outpost2.exe by address), runs script lines after the tick they're due, and writes `trace.jsonl` with the
  harness's field names, `harness.log` and BMP screenshots.
- **windows/trace_diff.py**: units matched by spawn index; the fields both traces write (`px`, `py`, `fix`, `dir`,
  `hp`, `lights`, `weapon`, `tur`, `reload`, `tgt`, `status`) and `rng`, tick by tick; reports the first tick each
  differs. `--ignore` sets known differences aside to see what's under them.
- **windows/compare_port.py**: original, port, trace_diff (and with `--op2rs` op2-rs's compare and picture), then
  the side-by-side PNG; several scenarios end in a one-line-each scoreboard.
- **run.py** (Linux): the full option set (`--mission`, `--campaign`, `--record`, `--resume`, `--checkpoint`,
  `--rng-log`...) under Wine and a private Xvfb, in its own prefix.
- **shell.py**, **netgame.py** (Linux): shell mode with X input, and multi-copy LAN games.

## 4. Data stores

No database. Everything is files:

- **Inputs:** scenario JSON, harness scripts (`.txt`), campaign carry-over files. op2-rs's are in its
  `harness/oracle/` (`$ORACLE`); any others work too.
- **Outputs (one folder per run, never committed):** `trace.jsonl`, `telemetry.jsonl`, `gamedata.json`,
  `harness.log`, `shot_<tick>.png`, `summary.json`, `scenario.json` and `scenario_script.txt` (what actually ran),
  and by option `audio.jsonl`, `events.jsonl` + `state.json`, `present.log`, `rng.log`, `recording.txt`,
  `shell.jsonl`.
- **A private `outpost2.ini` per run** in the output folder: the game's settings file is redirected there, so window
  sizes and sound settings are never written to the install's.
- **The game copy** (`game/`, Windows) or the Wine prefix (Linux), with `OPU/Harness/` holding the installed DLLs and
  the patched `zs_*.dll` copies.

Runs are deterministic, so recordings themselves aren't kept: the recipe is, and anyone with the game regenerates
identical bytes.

## 5. External integrations

- **Outpost 2** (GOG 1.2) with **OPU 1.4.1**. Every address in harness.cpp is for that `Outpost2.exe`; OPU's op2ext
  loads the module.
- **TethysAPI** 141fd47 (github.com/OutpostUniverse/TethysAPI, BSD-3): header-only C++ view of the game's classes
  and the mission API. Included as `Tethys/...`.
- **op2-rs** (gitlab.com/bufke/op2-rs, GPL-3.0): the source of this code, and the other side of every comparison.
  `op2-inspect` (`compare`: op2-sim against a trace) and `op2-engine` (renders a scenario) come from a built checkout
  (`compare.py --op2rs`).
- **outpost2-port** (github.com/leviathan400/outpost2-port, MIT; a fork of marcelomgoncalves/outpost2): its
  `--harness` mode is the other side of `compare_port.py`. Built with CMake + MSVC (Win32), installed into the game
  copy by `run_port.py install`. No code is shared: only the script and trace formats.
- **Toolchains:** MSVC 2022 Build Tools (Windows) or `i686-w64-mingw32-g++` (Linux); Python 3.11+ for the scripts;
  Wine and Xvfb for the Linux launchers; PIL optional on Linux for PNGs.

## 6. Deployment and infrastructure

No services. Built locally (`windows\build.bat`, or `make` on Linux). "Installing" means copying the DLLs into a
game's `OPU/Harness/` and adding one line to its `OPU/outpost2.ini`; on Windows that is always a private copy of the
game. Source is on GitHub (github.com/leviathan400/OP2TestHarness); there is no CI yet (op2-rs issue #34 asks for a
DLL build, and optionally `make check` under Xvfb and Wine).

## 7. Security considerations

- The harness patches a running game's code in memory, so it only ever runs against a copy: `run_win.py setup` refuses
  to install into the source install, and the Linux side refuses the play prefix.
- Bot DLLs (`OP2H_BOT`) and mission DLLs run natively inside the game: only use ones you built or trust.
- No game data goes into version control: `game/`, `build/` and run outputs are ignored; scenarios are recipes.
- On native Windows a run takes focus, and `ParkCursor` moves the real mouse pointer to the screen corner.

## 8. Development and testing

- **Windows:** `windows\build.bat`, `python -I windows\run_win.py setup`, then `run` or `compare.py`
  (windows/README.md). Rebuild + `setup` after every change to `harness.cpp`.
- **Linux:** `make install`, `./run.sh ...`; `make check` (self-test mission + session checks), `make record-check`.
- **The test that matters:** a scenario recorded twice gives byte-identical `trace.jsonl`, and op2-sim reports
  `MATCH` against it. Verified on Windows 10 (2026-11-09) for line_fast, cmb_duel_laser and cmb_duel_rail_gun.
- **outpost2-port:** `run_port.py install` after each port build, then `compare_port.py`. Results (2026-11-09, port
  958323a): the eight cmb_duel_* scenarios match exactly; line_fast is a tick off where the path state is freed
  (windows/README.md has the three differences the first runs found and fixed).
- When adding a hook: cite the address it came from, check the expected bytes (`HookEntry`), keep the module inert
  without its environment variable, and add a scenario under `$ORACLE/scenarios/` that exercises it.
- Clean-room rule inherited from op2-rs: behaviour is learned from disassembly and measurement, not pasted decompiled
  code.

## 9. Future considerations

- **Issue #21 in op2-rs:** a full Windows launcher. run_win.py covers scenarios only; missions, campaigns, recordings,
  checkpoints, shell mode and network games still need Windows versions of run.py, shell.py and netgame.py.
- **Window and focus on a real desktop:** the window opens at 0,0 with a 1032x766 client area (the ini padding is
  Wine's), takes focus, and the pointer is parked. Wanted: an off-screen window, no `SetCursorPos`, correct padding.
- **Crash dialogs:** Windows Error Reporting can hold a crashed run until the timeout; `SetErrorMode` in `InitMod`
  would end it at once.
- **Checkpoint state check** (`CheckDllState`) was tuned on Wine's memory layout; unmeasured on Windows and on ARM64.
- **op2-rs layout:** the Linux launchers look for the Wine prefixes beside this folder, and two tests for op2-inspect
  in `../tools` (PROVENANCE.md). Settings for those would make the Linux side independent of where the folder sits.
- **outpost2-port's differences:** triage each one as a port difference, a harness-mode mapping mistake or a
  deliberate fix of the port's (it fixes the original's defects rather than reproduce them), and keep a list of the
  deliberate ones. Its harness mode covers scenarios only, and its trace has no path detail, status timers or
  action names yet.
- **Keeping in step with op2-rs:** this started from op2-rs f7e9a15a; harness fixes made there need merging by hand.

## 10. Project identification

- **Name:** OP2TestHarness (Outpost 2 Test Harness).
- **Repository:** github.com/leviathan400/OP2TestHarness. Origin: gitlab.com/bufke/op2-rs, `harness/`, with its
  history (PROVENANCE.md).
- **Authors:** David Burke and op2-rs contributors (the harness); Leviathan (leviathan400), the Windows additions.
- **Last updated:** 2026-11-09.

## 11. Glossary

- **op2ext / module:** OPU's mod loader and the DLLs it loads from `[ExternalModules]` in `OPU/outpost2.ini`; it calls
  their `InitMod`, `RunMod`, `DestroyMod`.
- **OPU:** Outpost Universe's 1.4.x update of the game.
- **Scenario:** a JSON recipe (map, seed, ticks, terrain overrides, spawns, orders) both games can play.
- **Harness script:** `<tick> <verb> <args>` lines the DLL runs at those ticks; scenarios are translated into one.
- **Oracle / run:** a recording of the original playing a recipe; the reference op2-rs is compared against.
- **Trace:** per-unit state every N ticks (`trace.jsonl`), the field-by-field comparison's input.
- **Turbo:** frame limiter patched out and drawing skipped between captures: thousands of ticks per second.
- **Shell mode:** the game boots normally and the harness only watches (`OP2H_SHELL=1`).
- **Tick, mark:** game time; 100 ticks are a mark, and the game's speed setting decides how many ticks run per second.
- **Prefix:** a Wine prefix, the fake Windows folder the Linux launchers run the game in.
- **op2-sim, op2-engine, op2-inspect:** op2-rs's simulation, its game front end, and its inspection/compare tool.
- **outpost2-port:** a C++17/SDL2 reimplementation (`outpost2_modern.exe`) that runs the original mission DLLs.
- **Spawn index:** a unit's place in the scenario's spawn list (`@N` in scripts); how traces from different games
  are matched, since unit ids can differ.
