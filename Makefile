# Builds the harness op2ext module and installs it into the game's OPU folder.
#   make            build build/harness.dll
#   make install    copy it to OPU/Harness/ and register it (opt-in at runtime via OP2H_MISSION)
#   make uninstall  remove it and its ini registration
#   make check      end-to-end self-test (needs Xvfb and Wine; run outside the Claude sandbox)
# Also builds the sample bot (build/sample_bot.dll), the easter egg bot (build/easter_bot.dll, see bots/easter_bot.cpp), the cheat code bot (build/cheat_bot.dll), the self-test mission (build/zHarnessEnd.dll) and the scenario
# mission used by `run.sh --scenario` (build/zScenario.dll, installed next to harness.dll, not in OPU/maps).

CXX      := i686-w64-mingw32-g++
# Install into the prefix run.py runs in (harness/prefixes.py): OP2_HARNESS_PREFIX when set (an absolute path: run.py
# would read a relative one from the current directory, this file from harness/), else the harness's own prefix when it
# exists. prefix-harness is shared by every checkout's harness runs: an install replaces their DLLs. Never the play
# prefix (David's prefix/, this checkout's or the main checkout's): install and uninstall refuse it.
ifneq ($(OP2_HARNESS_PREFIX),)
ifeq ($(filter /%,$(OP2_HARNESS_PREFIX)),)
$(error OP2_HARNESS_PREFIX must be an absolute path, not $(OP2_HARNESS_PREFIX))
endif
endif
HARNESS_PREFIX ?= $(or $(OP2_HARNESS_PREFIX),$(if $(wildcard ../prefix-harness),../prefix-harness,../prefix))
PLAY_PREFIXES := $(realpath ../prefix $(shell git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)/../prefix)
GAME_DIR := $(HARNESS_PREFIX)/drive_c/GOG Games/Outpost 2
OPU_DIR  := $(GAME_DIR)/OPU

# Same TethysAPI/MinGW workarounds as missions/Makefile.
CXXFLAGS := -std=c++17 -O2 -Wall -Wno-return-type -Wno-array-bounds \
            -include src/mingw_compat.h \
            '-DBEGIN_PACKED=_Pragma("pack(push, 1)")' '-DEND_PACKED=_Pragma("pack(pop)")' \
            -isystem ../vendor/include
LDFLAGS  := -shared -static -static-libgcc -static-libstdc++ -s -Wl,--kill-at -Wl,--no-insert-timestamp -lgdi32

all: build/harness.dll build/sample_bot.dll build/easter_bot.dll build/cheat_bot.dll build/cursor_bot.dll build/hpblink_bot.dll build/hitmask_bot.dll build/los_bot.dll build/edge_bot.dll build/zHarnessEnd.dll build/zScenario.dll

build/harness.dll: src/harness.cpp src/mingw_compat.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

build/%.dll: bots/%.cpp src/mingw_compat.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

build/zHarnessEnd.dll: test/EndTest.cpp src/mingw_compat.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

build/zScenario.dll: test/Scenario.cpp src/mingw_compat.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# Refuses the play prefix (realpath: through the worktrees' links).
prefix-guard:
	@$(if $(filter $(realpath $(HARNESS_PREFIX)),$(PLAY_PREFIXES)),echo "harness: $(HARNESS_PREFIX) is the play prefix (David's): \
	  create prefix-harness/ or set OP2_HARNESS_PREFIX to a copy of your own" >&2; exit 1,true)

install: all prefix-guard
	@echo "harness: installing into $(HARNESS_PREFIX) (OP2_HARNESS_PREFIX or HARNESS_PREFIX chooses another)"
	mkdir -p "$(OPU_DIR)/Harness"
	# Copy then rename: a game that is running from this prefix keeps its mapped copy of the old file intact.
	for f in harness.dll zScenario.dll; do cp build/$$f "$(OPU_DIR)/Harness/.$$f.new" && \
	  mv -f "$(OPU_DIR)/Harness/.$$f.new" "$(OPU_DIR)/Harness/$$f"; done
	python3 tools/ini.py install "$(OPU_DIR)/outpost2.ini"

uninstall: prefix-guard
	python3 tools/ini.py uninstall "$(OPU_DIR)/outpost2.ini"
	rm -rf "$(OPU_DIR)/Harness"

# End-to-end self-test: run a mission that wins at tick 800 and check the harness saw it, then the session state and
# op2-inspect's game_* tools on it (test/session_check.py). The test mission is only
# present in OPU/maps while the check runs, so it never shows up in the in-game colony list.
check: install
	mkdir -p "$(OPU_DIR)/maps/HarnessTest"
	cp build/zHarnessEnd.dll "$(OPU_DIR)/maps/HarnessTest/"
	./run.sh --mission zHarnessEnd.dll --ticks 3000 --screenshot-every 400 --timeout 120 --out build/check && \
	  python3 -c "import json; s = json.load(open('build/check/summary.json')); \
	  assert s['game_term_reason'] == 'MissionAccomplished' and s['screenshots'] == 4, s; print('check: OK')" && \
	  python3 test/session_check.py; \
	  status=$$?; rm -rf "$(OPU_DIR)/maps/HarnessTest"; test $$status = 0

# The play-and-record mode (README "Recording your own play"): a short e01 session played with real X input under
# Xvfb, with a save and a load, replayed and compared (about a minute; not part of `check`: it plays in real time).
record-check: install
	python3 test/record_check.py --out build/record_check

clean:
	rm -rf build

.PHONY: all install uninstall check record-check clean prefix-guard
