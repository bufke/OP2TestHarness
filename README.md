# Outpost 2 test harness

Runs an Outpost 2 mission unattended under Wine and Xvfb: no shell or clicking, game speed uncapped, telemetry
written as JSONL, screenshots taken as PNG, and the game quits by itself. Any mission DLL works without changes.

```sh
make -C harness install                       # once (and after changing src/)
harness/run.sh --mission cHello.dll --ticks 6000 --screenshot-every 1500 --telemetry-every 500 --out /tmp/op2run
harness/run.sh --scenario harness/oracle/scenarios/line_fast.json --out /tmp/op2scn   # oracle trace of a scenario
make -C harness check                         # end-to-end self-test
```

When run from Claude Code, `run.sh` and `make check` need `dangerouslyDisableSandbox` (Wine uses `/run/user/1000/wine`).

## Options (`run.sh --help`)

| option | default | |
|---|---|---|
| `--mission NAME` | | mission DLL file name, found the same way the game finds it (`OPU/maps/*/`, game dir) |
| `--scenario FILE` | | scenario JSON instead of a mission (see "Scenarios"); exactly one of the two is required |
| `--out DIR` | required | output directory; previous outputs in it are replaced |
| `--ticks N` | 3000 | quit after game tick N (100 ticks = 1 mark). 0 = run until the mission ends. Default: the scenario's `ticks` |
| `--trace-every N` | 0, or 1 with `--scenario` | `trace.jsonl` line every N ticks (sub-pixel unit positions, see below) |
| `--trace-raw` | | add raw MapObject (0x80) and PathContext (0xD4) bytes as hex to each trace unit |
| `--screenshot-every N` | 0 (off) | PNG at tick 5, then every multiple of N, plus one at the end |
| `--telemetry-every N` | 100 | JSONL record at tick 1, then every multiple of N, plus one at the end. 0 = off |
| `--script FILE` | | scripted commands, see below |
| `--bot DLL` | | bot DLL called every tick, see below |
| `--seed N` | 1 | random seed (default: the scenario's). Identical seed and inputs give byte-identical telemetry |
| `--difficulty 0/1/2` | 1 | easy / normal / hard |
| `--no-turbo` | | keep the game's frame limiter and render every frame (real time at `--speed`) |
| `--speed 1-10` | 10 | in-game speed setting. Only matters with `--no-turbo` |
| `--draw-every N` | 0 | in turbo mode, also render every N ticks. Default: render only just before screenshots |
| `--size WxH` | 1024x768 | requested client size. The game rounds it (1024x768 gives 1032x766) |
| `--timeout S` | 600 | wall-clock limit. On timeout the X screen is saved to `timeout_screen.png` and the game is killed |
| `--display :N` | private Xvfb | use an existing X display instead, e.g. to watch a run |

The exit status is 0 when the run reached its tick limit or the mission ended.

## Outputs

- `summary.json`: exit code, whether it timed out, wall time, last tick, how the game ended (`Running`,
  `MissionAccomplished`, `MissionFailed`, ...), screenshot count, and any game dialogs that were suppressed.
- `telemetry.jsonl`: one object per sample:
  ```json
  {"tick":3000,"mark":30,"end":0,
   "players":[{"id":0,"human":1,"eden":1,"food":2528,"commonOre":0,"rareOre":0,"workers":20,"scientists":10,
               "kids":10,"morale":0,"moraleLevel":4,"powerGen":300,"powerUsed":70,"foodNet":-40,"buildings":4,
               "idleBuildings":0,"techs":[0,2099,2101,...]}],
   "units":[{"id":7,"type":"LIGHT_TANK","owner":0,"x":79,"y":61,"hp":300,"maxHp":300,"cmd":"Attack","cargo":"LASER"}]}
  ```
  - `x`/`y` are internal tile coordinates, the same `Location` values mission code uses. On non-wrapping maps the
    in-game display shows x-31, y+1.
  - `moraleLevel` runs from 0 (Great) to 4 (Rotten). `end` is the raw `GameTermReasons` value.
  - `techs` lists the tech IDs the player has completed.
  - `cmd` is the unit's current `CommandType`. `cargo` is the weapon type, or the kit a ConVec carries.
  - `units` holds every live building and vehicle of each player. Gaia objects such as beacons and wreckage are left
    out.
- `trace.jsonl` (with `--trace-every`/`--scenario`): the ENGINE_PLAN.md trace format, one line per sampled tick:
  ```json
  {"tick":120,"units":[{"id":1,"spawn":0,"type":"LIGHT_TANK","owner":0,"px":1680,"py":976,"sx":1679.666656,
   "sy":976.0,"fix":[110078635,63963136],"dir":0,"hp":300,"cmd":"Move","action":"Move","busy":0,"ipx":2704,
   "ipy":976,"frame":0,"anim":0,"dest":[42,22],"path":{"start":[1648,976],"moved":[10922,32768],"toMove":[1,0],
   "rotSpeed":0,"wp":0,"numWp":1,"pt":2,"numPts":30,"steps":"0000000000000000000000000000"}}]}
  ```
  - Units spawned by the script come first, in spawn order, with `spawn` = spawn index. Without spawns every vehicle
    is listed.
  - `px`/`py`: the game's integer `pixelX_`/`pixelY_` in **world pixels** (file tile * 32 + offset, see
    "Coordinates"). `fix` is the exact 16.16 fixed-point position and `sx`/`sy` the same as floats (see below).
    `ipx`/`ipy` are the raw internal pixels.
  - `frame`/`anim` are the sprite animation fields. The renderer sets them, so they depend on whether and when
    frames were drawn. Everything else is unaffected by rendering.
  - `dir` is `rotation_`, 0..255, 0 = east, clockwise. `cmd`/`action` are CommandType/ActionType names.
  - `dest` is the current waypoint's tile (file coordinates). `path` is the raw PathContext: `steps` holds the
    directions still planned in the current path fragment (one digit per tile, 0 = E, clockwise), `pt`/`numPts` the
    position in it, `rotSpeed` the per-tick rotation while turning.
  - `tiles`: the file tiles within 3 of the unit whose TileData::unitIndex is the unit (a building's footprint
    without its dock, a moving vehicle's next tile).
  - A spawn is listed until it dies (the dead flag), and never again: the game recycles object indices, so a dead
    spawn's ID can later belong to a shot or an explosion.
  - Armed units add `weapon`, `tur`, `reload`, `tgt` (ENGINE_PLAN.md). Top level: `fx` (weapon fire of spawned
    units, with `timer`), `ents` (every other live object as `[index, type, action, timer, px, py]`: explosions,
    unspawned units), `alloc` (the object index allocator, 0x439A10: highest index so far and the ring of freed
    objects as `[index, tick freed, reference count]`), `rng` (the gameplay RNG).
- `gamedata.json`, written at the first cycle: map geometry (`width`, `height`, `paddingTiles`, `clipRect`...), the
  game's cell type table (`trackSpeed` per track type: Legged, Wheeled, Miner, Tracked, Hover, `blocksLineOfSight`),
  every unit type's player-0 stats (`moveSpeed`, `turnRate`, `track`, `hp`, `armor`, the raw `stats` words, ...;
  buildings: `size`, `dock` per colony, `rubble` [common, rare], `buildingFlags`; weapons: `pixelsSkipped`,
  `radius`, `indirect` = type +0x250), and `cells`: the cell type of every tile as the running game sees it, in file
  coordinates, row-major (`op2-inspect verify_coords` checks it against the .map).
- `shot_<tick>.png`: the game window's client area.
- `harness.log`: what the in-game module did, including script commands, suppressed dialogs, the quit reason and the
  tick rate.
- `wine.log`, and `outpost2.ini`: the private settings copy used for the run (see below).

## Scripted input (`--script`)

One command per line: `<tick> <verb> <args>`. Text after `#` is ignored. Unit IDs are the `id`s from the telemetry,
or `@N` for the unit made by the Nth `spawn` command. Coordinates are internal tile coordinates unless a
`coords file` line switched to file coordinates. Example: `test/hello_script.txt`.

```
100  move      7 90 60      # unit 7 to tile (90,60)
100  dock      7 41 41      # Dock order (ctMoDock) to a tile, e.g. a structure's dock
3200 attack    8 11         # unit 8 attacks unit 11
0    coords    file         # later x/y are file tiles (internal x = file x + map padding)
0    spawn     LIGHT_TANK 0 20 30 LASER 64   # type, owner, x, y, weapon (or -), rotation 0-255
0    setcell   10 20 50 24 7                 # x1 y1 x2 y2 cellType: repaint terrain (inclusive)
10   move      @0 40 30     # the first spawned unit
0    view      60 64        # centre the view on a tile (for screenshots)
     stop <id> | selfdestruct <id> | research <labId> <techId> <scientists> | shot | telemetry | trace | quit
```

Commands for a tick run after that game cycle. A move at tick T shows `cmd: Move` in tick T's trace line and the unit
starts turning or moving in cycle T+1.

## Mouse and keyboard input

Script verbs that drive the game's UI the way a player does (the orders above bypass the UI):

```
10  click    unit @0              # left click on a unit (select it)
20  click    tile 34 28 +shot     # left click on a tile; +shot = screenshot one tick later
30  rclick   432 416              # right click at game window client pixels (as in screenshots)
40  drag     tile 22 22 tile 28 28          # left-button drag (box select); `right`/`middle` for other buttons
50  click    unit @1 shift        # modifiers: shift, ctrl, alt (held while the button goes down)
60  click    mini 50 50 hold 2    # minimap pixel showing tile (50,50); release the button 2 ticks later
70  click    ui tab2              # command pane tab 0-5 (Factories, Labs, Resources, Communications, Space, Game Options)
80  click    ui Self_Destruct     # a control by tooltip substring ('_' = space); `uilist` logs every control
90  key      ctrl+1 alt+c f5 enter          # key presses, through the frame's accelerators like real input
100 type     hello world          # characters (chat: `key enter`, `type ...`, `key enter`)
    mouse <pos> | down <pos> [button] | up <pos> [button] | dblclick <pos> | mclick <pos> | keydown <key> | keyup <key>
    select <unit>                 # left click on the unit, centring the view first if it is off screen
```

- A `<pos>` is `<x> <y>` (client pixels), `tile <x> <y>` (centre of that tile in the detail pane's current view;
  `coords file` applies), `unit <id|@N>`, `mini <x> <y>`, or `ui <name>` (`tabN`, `minimapN`, or tooltip text).
- Keys: letters, digits, `f1`-`f12`, `enter`, `esc`, `space`, `tab`, `backspace`, `delete`, `home`, `end`, `pageup`,
  `pagedown`, arrows, `pause`, `plus`, `minus`, `numplus`, `numminus`, `vk:0xNN`, with `shift+`/`ctrl+`/`alt+`.
- With `--scenario`, `--script` lines are appended to the scenario's own script (so in file coordinates), and a
  scenario may carry raw script lines as `"script": ["10 click unit @0", ...]`. `oracle/input/` has the controls
  experiments (`ui_base.json` plus one script per behaviour, see oracle/FINDINGS.md "Controls"):
  `harness/run.sh --scenario harness/oracle/input/ui_base.json --script harness/oracle/input/box_rules.txt
  --telemetry-every 1 --out /tmp/x`.
- When a script uses input verbs, each telemetry line adds `selected` (unit IDs, the game's selection list at
  0x547210) and `view` (the detail pane's top-left corner in file pixels). `harness.log` logs every input with the
  pane and map tile it hit, plus the pane rectangles.

How it works, and what is deterministic:
- Input runs in process right after the game cycle of its tick, like everything else in the script. Mouse messages
  go straight to the pane under the cursor (or the one with capture); key and character messages go through
  `GameFrame::TranslateAccelerators` and `IDlgWnd::PretranslateModeless`, then to the frame, as `TApp::PumpMessage`
  would. The game also polls `GetCursorPos` and `GetAsyncKeyState`; their IAT slots point at the virtual cursor and
  keys, so nothing depends on the X pointer (which is parked off the window). Same script, same output: repeated and
  parallel runs give byte-identical telemetry and traces. With `--no-turbo` the same orders land on the same ticks
  (only the draw-dependent `frame`/`anim` trace fields differ, as always).
- Orders made through the UI are not immediate: a click at tick T executes in the first cycle 4k+1 after T+8 (9 to
  12 ticks later), where the `move` verb executes at once. Selection changes are immediate.
- Some UI only advances when the panes are drawn: the view change from `view` or a minimap click, arrow-key and
  edge scrolling, the bandbox. In turbo mode the harness draws for 4 ticks around every input (and every `view` in an
  input run), so give a `view` a tick before inputs that depend on it, and hold the minimap button over a tick
  (`hold 1+`). Scroll distances depend on how many frames are drawn; use `--draw-every 1` for realistic ones.
- Input runs keep the game window active (parallel runs share a wineserver and take the foreground from each other;
  an inactive game ignores keys and scrolling).
- Don't pause the game (`ctrl+p`, `pause`): no cycles run while paused, so the script can't unpause it and the run
  ends at `--timeout`.

## Economy verbs and telemetry (`--eco`)

Script verbs for colony experiments (oracle scenarios `eco_*`, spec in `oracle/ECONOMY.md`):

```
1    player   0 workers 40          # set a PlayerImpl field: workers scientists kids food common rare morale
                                    # moraleLevel solarsat, or a byte offset (0x94); sets recalculateValues_
0    tube     44 47 69 47           # tubes over an inclusive rectangle (Game::CreateWall); `wall x1 y1 x2 y2 [TYPE]`
0    beacon   40 60 0 1 2           # mining beacon: ore 0 common / 1 rare, bar 0-2 (0 = 3-bar), internal variant 0-2
0    beacon   44 60 vent            # magma vent (`fumarole` for a fumarole)
5    route    @3 @1 @2              # cargo truck @3 on a mine (@1) <-> smelter (@2) route, as the UI sends it
5    produce  @4 CARGO_TRUCK        # factory order (TYPE [weapon]); `idle`/`unidle <bldg>`, `train <university> <n>`
5    kit      @5 TOKAMAK            # put a structure kit in a ConVec; `build <convec> <x> <y>` (footprint bottom-right)
5    bay      @4 0 TOKAMAK          # fill a factory bay; `transfercargo <factory> <bay>`
5    cargo    @3 2 1000             # set a truck's cargo (CargoType, amount); `load`/`unload <truck>`
5    poke     @2 0x1e 2 1200        # write a MapObject field (offset, size 1/2/4, value), e.g. damage
```

`--eco` (or `"eco": true` in a scenario) adds to each telemetry player an `eco` object (storage limits, available and
required workers/scientists, scientists working as workers, food production/consumption, growth and death remainders,
capacities, morale inputs) and `pi`, every dword of PlayerImpl 0x00..0x1EF; and to each unit `flags` (MapObject +0x44),
`act`/`timer` (action and its timer), `busy`, `w24` (the cargo/researchTimer word) and `raw` (bytes 0x48..0x7F), plus
`truck` [cargo type, amount] for Cargo Trucks. `gamedata.json` gets `morale` (levels and modifiers as loaded), `mines`
(the yield table) and `typeExtra` ([mapId, resource priority, research topic]). Scenarios may also set
`telemetryEvery`, `traceEvery` and `difficulty`. Runs without `--eco` produce the same output as before.

## Scenarios (`--scenario`): oracle runs of the original game

`--scenario FILE` runs a scenario in the ENGINE_PLAN.md format against the real game, so op2-sim can be compared with
it:

```json
{"map": "cm01.map", "seed": 1, "ticks": 1400,
 "setcell": [{"rect": [10, 20, 50, 24], "cell": 7}],
 "spawn": [{"type": "LIGHT_TANK", "owner": 0, "tile": [12, 22], "weapon": "LASER", "dir": 0}],
 "orders": [{"tick": 10, "unit": 0, "cmd": "Move", "tile": [42, 22]}]}
```

- Tiles are file coordinates. `type` and `weapon` are sheet code names. `dir` (optional) is the starting rotation.
  `orders[].unit` is a spawn index. `cmd` is Move, Dock (with `tile`), Attack (with `target`, a spawn index), Stop or SelfDestruct.
  `view` (optional tile, default the spawns' centroid) centres the camera for screenshots.
  `setcell` (optional) repaints cell types before spawning (`cell` is a CellType id or name). An older `cells` key is
  read too. Unknown keys such as `note` are ignored.
- run.py turns the scenario into a script (`scenario_script.txt` in the output) and runs the scenario mission
  (`test/Scenario.cpp`): an empty colony on `map`, with player 0 a human Eden player, player 1 an idle Plymouth AI,
  and daylight everywhere. Spawns happen at tick 1, the first harness cycle. The trace starts there and samples
  every tick.
- The game reads a mission's `MapName` from the DLL file without loading it (MissionManager::GetModuleDesc), so
  run.py writes one patched copy of `zScenario.dll` per map, as `OPU/Harness/zs_<map>.dll`. Missions in
  `OPU/Harness` never appear in the game's mission lists.
- A scenario run takes about 1 to 3 s. `oracle/` holds the reference scenarios (`make_scenarios.py` writes them;
  `run_all.sh [names]` runs them 4 at a time into `oracle/runs/`, which is gitignored, and `analyze.py` summarises
  steps, turns and routes). Measured results are in `oracle/FINDINGS.md` and `../notes/findings-*.md`. `op2-inspect`
  (tools/README.md) wraps all of this for agents.

Recording a scenario as video frames: `--screenshot-every 1` (or 2, 5...) saves a PNG per sampled tick of the view
centred on the units, about 20 frames/s of wall time. Assemble them with e.g.
`ffmpeg -framerate 25 -pattern_type glob -i 'shot_*.png' out.mp4`.

## Coordinates

- Internal tile x = file tile x + 32 on non-wrapping maps (`GameMap::GetPaddingWidth()`), and + 0 on 512-wide world
  maps, which wrap. Internal y = file y. Pixel = tile * 32 + offset, with unit centres at +16. The status bar shows
  file + 1 on both axes. This was verified for cm01 and newworld by comparing the game's whole cell grid with the
  .map file.
- Exact position: while a vehicle steps between tiles, `PathContext::numPixelsMoved{X,Y}` hold the 16-bit fraction
  of a 16.16 position, biased by 0x8000, around `pixelX_/pixelY_`. They read 0 when no step has started.

## Bot DLL (`--bot`)

A bot is a DLL that exports `extern "C" void __cdecl HarnessBotTick(int tick)`. The harness calls it after every game
cycle, and it can use all of TethysAPI to read state and give orders. `bots/sample_bot.cpp` sends player 0's idle
tanks at the nearest enemy. Build it with `make -C harness`, then run with `--bot harness/build/sample_bot.dll`.

`bots/easter_bot.cpp` reproduces Dan's Dog (`EGG_MODE=dog`, with `test/egg_dandog.json`) and shows the unused Santa
(`EGG_MODE=santa`, with `test/egg_santa.json`) in the original; see `../notes/findings-easter-eggs.md`.

All orders, from scripts or bots, should use command packets: `Unit::Do*` helpers that build a `CommandPacket`, or
`PlayerImpl::ProcessCommandPacket`. Avoid TethysAPI's `Unit::DoMove()`. It calls `MapObject::CmdMove()`, the AI/ScGroup
path, and on a human player's unit it crashed the game as soon as a frame was rendered. The harness `move` verb builds
the Move packet itself (see `IssueMove`).

Commands like these run immediately on the local player, which is fine in single player. In multiplayer they would
desync: real player input is queued through the network layer (`GameNetLayer`) and runs on every machine at the same
command-processing tick. A multiplayer-safe bot would have to queue packets there.

## How it works

`src/harness.cpp` builds `OPU/Harness/harness.dll`, an op2ext ini module listed in `OPU/outpost2.ini`
(`[ExternalModules] Harness=yes`). op2ext calls `InitMod` from its `TApp::Init` hook. If the environment has no
`OP2H_MISSION`, `InitMod` returns at once and the game behaves normally. Otherwise it:

1. Points the game's settings object (`g_configFile.iniPath_`) at a copy of `outpost2.ini` in the output directory,
   with music and sound off and window geometry set. The game writes its window and desktop size back to the ini while
   it runs, and this keeps those writes away from the user's settings.
2. Sets `TApp::bShowShell_ = 0`, the same thing the hidden `/noshell` switch does. `TApp::CreateFrames()` then calls
   `GameImpl::LoadDebugMap()` instead of opening the shell. The harness redirects that call (0x486606) to a function
   that fills a `GameStartInfo` (script name, seed, speed, difficulty) and calls `GameImpl::PrepareGame()`, which is
   what `TApp::StartSingleGame()` does when the shell starts a game.
3. Redirects the call to `GameImpl::ProcessGameCycle()` in `GameFrame::OnIdle()` (0x49C4BD). After each cycle it runs
   due script commands, calls the bot, writes telemetry and screenshots, and quits (`TerminateProcess`) at the tick
   limit or when `gameTermReasons_` leaves `Running`.
4. Turbo: changes the frame limiter's `jae` at 0x49C374 to `jmp`, so a game cycle runs on every idle loop. It also skips
   the three pane `OnIdle()` draw calls (0x49C5FC) except during the first ticks and the 4 ticks before each capture,
   because rendering costs about 10x more than simulating. Skipping draws does not change the simulation: telemetry
   with screenshots matches telemetry without them, byte for byte.
5. Replaces the game's modal message box (0x41E0E0) with a function that logs the message and returns OK. A bad mission
   name therefore ends the run with exit status 2 instead of hanging.

Startup is set up for single-player missions. Missions whose names start with `t` get the Tutorial type, all others
Colony. Campaign missions (which have `isCampaign` set, plus campaign state) and multiplayer types have not been
tried.

Measured speed with turbo depends on machine load and map size. Cases seen: a 16-unit map ran at about 500 to 50,000
ticks/s with occasional screenshots, about 100 ticks/s when rendering every tick, and 36 ticks/s at real-time speed 10.
Wine and Xvfb startup add about 3 s per run. Parallel runs work: each gets its own Xvfb and output directory, and they
share the harness prefix's wineserver.

**Own prefix.** When `prefix-harness/` exists (a plain copy of `prefix/`: `cp -a prefix prefix-harness`), `run.py` and
`make install` use it instead of `prefix/` (override with `OP2_HARNESS_PREFIX` / `HARNESS_PREFIX`). Harness runs then
have their own wineserver, so they can't take focus from, or replace DLLs under, a game being played from `prefix/`.
Mission DLLs are installed into `prefix/` by missions/Makefile; `run.py` mirrors `OPU/maps` into the harness prefix
before each run.

## Disabling / removing

- Normal play is unaffected while `OP2H_MISSION` is unset. To unregister the module completely:
  `make -C harness uninstall`. This removes the ini lines and `OPU/Harness/`.
- `OPU/outpost2.ini.pre-harness` is a backup of the ini taken before the first install.
- `make check` puts its test mission in `OPU/maps/HarnessTest/` only while the check runs, so it never appears in the
  in-game lists.

## Limitations

- The game rounds the window size, so `--size` is only approximate.
- If a mission ends on its own, the final screenshot may show a frame from up to `--screenshot-every` ticks earlier,
  because rendering was skipped. Add `--draw-every 50` (for example) if the last frame matters.
- Only the game's own message box is intercepted. A `MessageBoxA` from a mission DLL still blocks until `--timeout`,
  and `timeout_screen.png` shows it. A crash ends the run with a non-zero exit (winedbg is disabled for harness runs).
- Scenario runs are single player only. Spawns are vehicles and buildings of players 0 and 1 via
  `Game::CreateUnit`.
- Telemetry covers what is cheap to read, and is easy to extend in `WriteTelemetry()`. It does not cover research in
  progress, factory queues, cargo amounts, or disasters.
- The game's code addresses are hard-coded for Outpost2.exe 1.2 as patched by OPU 1.4.x. They are the same addresses
  TethysAPI and OPUPatch use.
