# Generated and maintained by Claude
#
# Shortcuts for the commands this project is actually driven by. Everything here is a wrapper
# around the same `./build/recoil-metal` invocations the README documents — nothing is only
# possible through make, and nothing here hides a flag you would want to know about. Every
# target echoes the command it runs, so a target is also documentation of the command.
#
# WHY THIS EXISTS: the two content families live at absolute paths on two different volumes,
# one of them removable, and a session of testing means retyping them dozens of times.
#
# WHERE THE PATHS COME FROM, in order — first one that is set wins:
#
#   1. the command line          make skirmish FA_ROOT=/Volumes/Other/FA
#   2. local.mk, if present      a gitignored file of `FA_ROOT = ...` lines
#   3. the environment           export RM_FA_ROOT=... (see below)
#   4. the defaults below
#
# local.mk beats the environment because it is the more specific of the two: an export is
# shell-wide while local.mk belongs to one checkout, and a checkout pointed at a particular
# install should keep pointing there whatever the shell says. That is also just what the code
# does — `-include` runs before the `?=` below, so an assignment in local.mk makes it a no-op —
# and the two agreeing is the point of saying so.
#
# The environment is the one worth setting up once. This machine keeps its shell config
# modular under ~/projects/llm/shell/, and `topics/recoil-metal.zsh` exports the roots as
# RM_FA_ROOT, RM_BAR_ROOT and RM_BAR_MAPS — prefixed so a bare FA_ROOT belonging to some other
# project cannot collide with them, and picked up here. On a machine without that config the
# defaults still work.
#
# Tools go through `mise exec --`, per this machine's convention.

# A gitignored file for machine-specific paths, for anyone who would rather not export them.
# `-include` so its absence is silent rather than an error.
-include local.mk

# --- Where the content lives -------------------------------------------------
#
# Supreme Commander: a retail Forged Alliance install. `gamedata/` holds the `.scd` archives
# the engine mounts and `maps/` the `.scmap` directories. An external drive here, so the
# targets that need it check first and say so rather than failing inside the engine.
FA_ROOT ?= $(if $(RM_FA_ROOT),$(RM_FA_ROOT),/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance)

# Beyond All Reason: FAR's reference tree for models and unit definitions, and a separate
# directory of extracted maps (see the README for how they were fetched).
BAR_ROOT  ?= $(if $(RM_BAR_ROOT),$(RM_BAR_ROOT),$(HOME)/projects/llm/games/forged-alliance-reborn/reference/BAR)
BAR_MAPS  ?= $(if $(RM_BAR_MAPS),$(RM_BAR_MAPS),$(HOME)/projects/llm/input/recoil/maps)

# The defaults each family is exercised with. SCMP_009 because it is the map every screenshot
# in the README was taken on; aw04 because it is the Recoil map the benchmarks are calibrated
# against.
FA_MAP  ?= $(FA_ROOT)/maps/SCMP_009/SCMP_009.scmap
BAR_MAP ?= $(BAR_MAPS)/aw04.smf

# A unit from each family, by the path each family names its content with: Supreme Commander
# by VFS path inside the mounted archives, Recoil by a real file on disk.
FA_UNIT  ?= /units/UEL0201/UEL0201_unit.bp
BAR_UNIT ?= $(BAR_ROOT)/units/ArmVehicles/armstump.lua

# --- Knobs -------------------------------------------------------------------
BUILD     ?= build
BIN       := ./$(BUILD)/recoil-metal
UNITS     ?= 40
SECONDS   ?= 30
SHOT      ?= /tmp/recoil-metal.png
SHOT_SIZE ?= 1400 900
MARCH     ?= 4096 4096

FA_FLAGS  = --gamedata "$(FA_ROOT)/gamedata"

.DEFAULT_GOAL := help
.PHONY: help build configure test run run-fa run-bar skirmish battle shot-fa shot-bar \
        bench bench-fa bench-gl clean check-fa check-bar

help:
	@echo 'recoil-metal — make targets'
	@echo
	@echo '  build           configure and build'
	@echo '  test            the whole suite'
	@echo
	@echo '  run             procedural terrain, no content needed'
	@echo '  run-fa          a Supreme Commander map, its own units, read from the archives'
	@echo '  run-bar         a Recoil map with Beyond All Reason units'
	@echo '  skirmish        eight armies, commanders, economy — a match you can watch'
	@echo '  battle          the same, marched to the middle and fought to a finish'
	@echo
	@echo '  shot-fa         one frame of the above, to $$SHOT'
	@echo '  shot-bar        the same for the Recoil path'
	@echo
	@echo '  bench-fa        offscreen benchmark on the Supreme Commander map'
	@echo '  bench           offscreen benchmark on the Recoil map'
	@echo '  bench-gl        the Recoil OpenGL baseline, for comparison'
	@echo
	@echo '  clean           remove the build directory'
	@echo
	@echo 'Override anything: make skirmish UNITS=200 SECONDS=90 FA_MAP=.../SCMP_012.scmap'
	@echo
	@echo 'Content:'
	@echo '  FA_ROOT   $(FA_ROOT)'
	@echo '  BAR_ROOT  $(BAR_ROOT)'
	@echo '  BAR_MAPS  $(BAR_MAPS)'
	@echo
	@echo 'Where those could have come from — local.mk first, then the environment:'
	@echo '  local.mk     $(if $(wildcard local.mk),present,absent)'
	@echo '  RM_FA_ROOT   $(if $(RM_FA_ROOT),$(RM_FA_ROOT),unset)'
	@echo '  RM_BAR_ROOT  $(if $(RM_BAR_ROOT),$(RM_BAR_ROOT),unset)'
	@echo '  RM_BAR_MAPS  $(if $(RM_BAR_MAPS),$(RM_BAR_MAPS),unset)'

# --- Build -------------------------------------------------------------------

configure:
	mise exec -- cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release

# Configures on first use and then just builds: cmake re-runs itself when CMakeLists.txt
# changes, so configuring every time would only cost seconds for nothing.
build:
	@test -f $(BUILD)/CMakeCache.txt || $(MAKE) configure
	mise exec -- cmake --build $(BUILD)

test: build
	mise exec -- ctest --test-dir $(BUILD) --output-on-failure

# --- Content checks ----------------------------------------------------------
#
# Named targets rather than a check inside each run target, so the message is about the
# content being missing rather than about a map failing to parse. The Supreme Commander
# install is on a removable drive and being unplugged is the ordinary case, not an error.

check-fa:
	@test -d "$(FA_ROOT)/gamedata" || { \
	  echo 'No Forged Alliance install at:'; \
	  echo '  $(FA_ROOT)'; \
	  echo 'Plug the drive in, or point it somewhere else — any of:'; \
	  echo '  make $(MAKECMDGOALS) FA_ROOT=/path/to/install    once'; \
	  echo '  export RM_FA_ROOT=/path/to/install               for good'; \
	  echo '  echo "FA_ROOT = /path/to/install" >> local.mk    for good, per checkout'; \
	  exit 1; }
	@test -f "$(FA_MAP)" || { echo 'No map at $(FA_MAP)'; exit 1; }

check-bar:
	@test -f "$(BAR_MAP)" || { \
	  echo 'No Recoil map at $(BAR_MAP)'; \
	  echo 'See the README for fetching Beyond All Reason maps, or set BAR_MAP.'; \
	  exit 1; }

# --- Running -----------------------------------------------------------------

# No content at all: procedural terrain. The one target that works on any machine, which makes
# it the first thing to try when something is wrong.
run: build
	$(BIN)

# Supreme Commander, read straight out of the `.scd` archives — nothing extracted.
run-fa: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --units $(FA_UNIT) $(UNITS)

run-bar: build check-bar
	$(BIN) "$(BAR_MAP)" --units "$(BAR_UNIT)" $(UNITS)

# A match: one army per start position, each with its faction's commander, each building an
# extractor on the map's own mass deposits.
skirmish: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish

# The same, fought. `--march` sends everything at one point and pre-runs the sim, so the
# result is the same every run — which is what makes a screenshot of it worth comparing.
battle: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish --march $(MARCH) $(SECONDS)

# --- Screenshots -------------------------------------------------------------
#
# Headless, so they work whichever Space is in front — the reason `--screenshot` exists.

shot-fa: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish --march $(MARCH) $(SECONDS) \
	  --screenshot $(SHOT) $(SHOT_SIZE)

shot-bar: build check-bar
	$(BIN) "$(BAR_MAP)" --units "$(BAR_UNIT)" $(UNITS) --march $(MARCH) $(SECONDS) --focus \
	  --screenshot $(SHOT) $(SHOT_SIZE)

# --- Benchmarks --------------------------------------------------------------
#
# `--bench-offscreen` is the comparable number: the windowed `--bench` is vsync-limited and
# only useful for eyeballing GPU milliseconds. Note the README's warning — alternate
# configurations rather than running all of A then all of B, because a batch drifts as the GPU
# warms.
FRAMES ?= 2060

bench: build check-bar
	$(BIN) "$(BAR_MAP)" --bench-offscreen $(FRAMES) docs/bench.csv

bench-fa: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --bench-offscreen $(FRAMES) docs/bench-fa.csv

bench-gl:
	tools/bench_recoil_gl.sh docs/bench-recoil-gl.csv

clean:
	rm -rf $(BUILD)
