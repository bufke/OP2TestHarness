# Provenance

OP2TestHarness is the original-game test harness of op2-rs (https://gitlab.com/bufke/op2-rs), moved to a repository of
its own as op2-rs issue #34 proposes. op2-rs is written by David Burke and contributors and licensed under the GNU GPL
v3; this project keeps that licence (LICENSE).

## History

The git history up to the merge "Merge the harness's history from op2-rs" is op2-rs's, filtered to its `harness/`
folder at op2-rs commit ab5f74dd (2026-11-09):

```
git clone https://gitlab.com/bufke/op2-rs.git op2-harness && cd op2-harness
git filter-repo --path harness/ --path-rename harness/:
git filter-repo --force --invert-paths --path oracle/
```

Authorship and dates are op2-rs's; `git log` and `git blame` follow every file back to op2-rs's first harness commit.

## Not here: `oracle/`

op2-rs's `harness/oracle/` (the scenarios, mission and input scripts, the measured findings and the recorded runs) stays
in op2-rs: they are its regression tests. The docs write that folder as `$ORACLE` (README.md "Scenarios and recorded
runs"). `test/session_check.py` reads two of its mission scripts from `$ORACLE` (default: `oracle/` in this folder,
as inside op2-rs).

## What this repository adds

- `windows/`: an MSVC build (`build.bat`, `msvc_compat.h`), a native Windows launcher (`run_win.py`), the original-vs-
  op2-rs runner (`compare.py`) and the outpost2-port comparison (`run_port.py`, `trace_diff.py`, `compare_port.py`).
- Docs for a project of its own: README.md's introduction, quick starts and "Comparing with op2-rs", ARCHITECTURE.md,
  this file. Paths are relative to this folder; op2-rs's files and tools are named as op2-rs's.
- The Makefile reads TethysAPI from `vendor/include` here (then `../vendor/include`); run.py compares the installed
  DLLs with this folder's `build/`.
- `src/harness.cpp`'s behaviour is op2-rs's; only comments and the recording header's command line differ.

## What still follows op2-rs's layout

- The Linux launchers find the Wine prefixes beside this folder (`../prefix`, `../prefix-harness`) unless
  `OP2_HARNESS_PREFIX` is set: in op2-rs that folder is the repository root.
- `test/session_check.py` and `test/record_check.py` use op2-inspect from an op2-rs build beside this folder
  (`../tools/target/debug`); without one, session_check skips that part with a note.
- The scenario format and trace fields are defined in op2-rs's ENGINE_PLAN.md; README.md "Outputs" and "Scenarios"
  describe what this harness writes and reads.

## The contract op2-rs depends on

op2-rs issue #34 lists what its 1,000+ recorded runs need to stay the same: the `OP2H_*` variables and `run.sh` options,
the script verbs and their meaning, unit names (`$NAME`, `as NAME`, `$TYPE@x,y`), the output fields (telemetry, trace,
`start_info.json`, audio log, screenshots) and the scenario JSON format. Changing any of them needs a version bump and a
changelog note; adding verbs and options is fine. CHANGELOG.md "Versioning rules" says how each kind of change moves
the version.
