# Changelog

All notable changes to OP2TestHarness. Versions follow [Semantic Versioning](https://semver.org/); the version is in
`VERSION`, and each release is tagged `vX.Y.Z`.

## Versioning rules

op2-rs and other users replay recorded runs against this harness, and those recordings depend on **the contract**:

- **Environment and options:** the `OP2H_*` environment variables and the `run.sh` / `run.py` options.
- **Script verbs and their meaning:** README.md "Scripted input" and the sections after it.
- **Unit names:** `$NAME`, `as NAME` and `$TYPE@x,y`.
- **Output fields:** the telemetry and trace JSONL fields, `gamedata.json`, `start_info.json`, the audio log and the
  screenshots.
- **The scenario JSON format.**

How a change moves the version:

- **MAJOR:** a change to the contract that can change a recorded run's output or break an existing script or option:
  a renamed or removed variable, option, verb or field, a verb or field whose meaning changes, a different screenshot.
- **MINOR:** additions that leave every existing recording replaying identically: a new verb, option, variable or output
  field, a new launcher or platform (e.g. a Windows launcher).
- **PATCH:** fixes and changes that don't touch the contract: docs, build scripts, launcher fixes, crash fixes in paths no
  recording uses.

Every release notes its contract changes, if any, under **Contract**. A release is only made from a commit whose
`harness.dll` replays op2-rs's recorded runs identically (op2-rs's `make accuracy`, 0 failures) unless its changelog
says which runs change and why.

## 1.0.0 (not yet released)

The first version as a repository of its own (op2-rs issue #34).

**Contract:** identical to op2-rs's `harness/` at commit ab5f74dd (2026-11-09): the same `OP2H_*` variables, `run.sh`
options, script verbs, unit names, output fields and scenario format. `src/harness.cpp` behaves as op2-rs's; it differs
only in comments and in the command line written into a recording's header.

**History:** op2-rs's `harness/` history, filtered with `git filter-repo` and without `oracle/` (PROVENANCE.md).

**Added**
- `windows/`: an MSVC build of `harness.dll` and `zScenario.dll` (`build.bat`, `msvc_compat.h`); a native Windows
  launcher that plays scenarios in a private copy of the game, without Wine (`run_win.py`); the original vs op2-rs on one
  scenario, with op2-sim's tick-by-tick compare and both pictures side by side (`compare.py`); the original vs
  outpost2-port (`run_port.py`, `trace_diff.py`, `compare_port.py`).
- `ARCHITECTURE.md`, `PROVENANCE.md`, this changelog, `VERSION`, `.gitignore`.

**Changed**
- README.md describes a project of its own: Windows and Linux quick starts, requirements, where prefixes and the game
  copy live, and "Comparing with op2-rs". op2-rs's scenarios stay in op2-rs (`harness/oracle/`), written `$ORACLE`.
- `test/session_check.py` reads the canonical campaign scripts from `$ORACLE` (default `oracle/` in this folder, as
  inside op2-rs).
- The Makefile reads TethysAPI from `vendor/include` in this folder first; `run.py` compares the installed DLLs with this
  folder's `build/`.

**Verified** (Windows 10, MSVC build): line_fast, cmb_duel_laser and cmb_duel_rail_gun match op2-sim on every trace
field of every tick, and two runs of a scenario give byte-identical traces. Not yet verified: the MinGW build since the
move, and op2-rs's full `make accuracy` against this version.
