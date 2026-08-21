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
BAR_ROOT  ?= $(if $(RM_BAR_ROOT),$(RM_BAR_ROOT),$(HOME)/projects/llm/games/faf/forged-alliance-reborn/reference/BAR)
BAR_MAPS  ?= $(if $(RM_BAR_MAPS),$(RM_BAR_MAPS),$(HOME)/projects/llm/input/recoil/maps)

# The defaults each family is exercised with. SCMP_009 because it is the map every screenshot
# in the README was taken on; aw04 because it is the Recoil map the benchmarks are calibrated
# against.
FA_MAP  ?= $(FA_ROOT)/maps/SCMP_009/SCMP_009.scmap
BAR_MAP ?= $(BAR_MAPS)/aw04.smf

# A unit from each family, by the path each family names its content with: Supreme Commander
# by VFS path inside the mounted archives, Recoil by a real file on disk.
FA_UNIT  ?= /units/UEL0201/UEL0201_unit.bp
# The UEF T1 engineer. `make shot-engineer` needs a BUILDER on the map and a scripted match
# never produces one — see that target.
FA_ENGINEER ?= /units/UEL0105/UEL0105_unit.bp
BAR_UNIT ?= $(BAR_ROOT)/units/ArmVehicles/armstump.lua

# --- Knobs -------------------------------------------------------------------
BUILD     ?= build
BIN       := ./$(BUILD)/recoil-metal
UNITS     ?= 40
SECONDS   ?= 30
# How many units the capture rings, which is also what the BUILD PANEL is drawn for: a headless
# run has no clicks, so `--select` is the only way an interface that appears on selection can be
# screenshotted at all. `make shot-fa SELECT=1` captures a commander's build list.
SELECT    ?= 0
SELECT_FLAG = $(if $(filter-out 0,$(SELECT)),--select $(SELECT),)
SHOT      ?= /tmp/recoil-metal.png
SHOT_SIZE ?= 1400 900
MARCH     ?= 4096 4096

# Whether terrain blocks sight (ADR-037): `recoil` or `fa`. Empty leaves the binary's own
# default, which is recoil's.
VISION    ?=
VISION_FLAG = $(if $(VISION),--vision-style $(VISION),)

FA_FLAGS  = --gamedata "$(FA_ROOT)/gamedata"

.DEFAULT_GOAL := help
.PHONY: help build configure test verify golden play play-from watch run run-fa run-bar \
        skirmish battle match shot-fa shot-bar bench bench-fa bench-gl clean check-fa check-bar \
        ai ai-play ai-report shot-ui shot-engineer

help:
	@echo 'recoil-metal — make targets'
	@echo
	@echo '  build           configure and build'
	@echo '  test            the whole suite'
	@echo '  verify          replay the golden match — MATCH, or the tick it broke'
	@echo '  golden          re-record it (only when the change was meant to alter the match)'
	@echo '  ai              fetch the vendored AI corpora at their pins (FORCE=1 to re-fetch)'
	@echo
	@echo '  PLAY IT'
	@echo '  play            a duel you drive — army 0 is yours (ARMIES=8 for a free-for-all)'
	@echo '  play-from       the same, joining SECONDS in, so a base already stands'
	@echo '  watch           every side scripted, nothing selectable (and no fog)'
	@echo '  ai-play         bots vs bots, with the FAF AI sandbox report on the console'
	@echo
	@echo '  run             procedural terrain, no content needed'
	@echo '  run-fa          a Supreme Commander map, its own units, read from the archives'
	@echo '  run-bar         a Recoil map with Beyond All Reason units'
	@echo '  skirmish        eight armies, commanders, economy — a match you can watch'
	@echo '  battle          the same, marched to the middle and fought to a finish'
	@echo
	@echo '  shot-fa         one frame of the above, to $$SHOT'
	@echo '  shot-bar        the same for the Recoil path'
	@echo '  shot-ui         a commander selected: the build panel and minimap in frame'
	@echo '  shot-engineer   the same for an engineer, which a scripted match never builds'
	@echo
	@echo '  bench-fa        offscreen benchmark on the Supreme Commander map'
	@echo '  bench           offscreen benchmark on the Recoil map'
	@echo '  bench-gl        the Recoil OpenGL baseline, for comparison'
	@echo
	@echo '  clean           remove the build directory'
	@echo
	@echo 'Override anything: make play ARMIES=8 ALLIANCES=2 FA_MAP=.../SCMP_012.scmap'
	@echo '                   make play VISION=fa    (flat discs, as Supreme Commander does)'
	@echo '                   make skirmish UNITS=200 SECONDS=90'
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

# --- The AI corpora ----------------------------------------------------------
#
# The FAF and Circuit trees the adapters are written against (ADR-039), fetched at pinned
# commits into vendor/ai/, which is gitignored. Not needed to build or to play — only to work
# on an adapter — so this is a target rather than a CMake check.
#
# The pins, the path lists and the reasoning all live in the script; this is a shortcut, not a
# second place to look.
ai:
	tools/fetch_ai.sh $(if $(FORCE),--force,) $(if $(YES),--yes,)

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

# --- Playing it -------------------------------------------------------------
#
# `make play` is the one to run. Everything else in this file exercises a slice; this opens
# the game.
#
# ARMIES defaults to 2 because a duel is what the scripted opponent plays well — it builds an
# opening, masses a wave and attacks once. Raise it for a free-for-all: the map declares eight
# start positions, so `make play ARMIES=8` fills them and `ALLIANCES=2` makes it 4v4.
#
# You drive ARMY 0 — `--skirmish` seats a human there and gives every other army a script
# (`onePlayerPerArmy(.., humanArmy=0)`), so your units are the only ones a left click selects.
ARMIES    ?= 2
ALLIANCES ?= 0
ALLIANCE_FLAG = $(if $(filter-out 0,$(ALLIANCES)),--alliances $(ALLIANCES),)

play: build check-fa
	@echo
	@echo '  You are army 0. WASD pans, left-click selects, right-click orders.'
	@echo '  Shift + right-click queues an order; hold space and drag to swing the camera.'
	@echo '  Scroll zooms. Losing your commander loses the match.'
	@echo
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish --armies $(ARMIES) $(ALLIANCE_FLAG) $(VISION_FLAG)

# The same match with the first SECONDS already played out, so you arrive at a base rather
# than at two commanders on empty ground. 60 is about when the factory is up; 320 is just
# before the attack wave leaves.
play-from: build check-fa
	@echo
	@echo '  You are army 0, joining at $(SECONDS)s. WASD pans, right-click orders.'
	@echo
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish --armies $(ARMIES) $(ALLIANCE_FLAG) $(VISION_FLAG) \
	  --play $(SECONDS)

# Watch instead of play: no army is yours, so nothing is selectable and every side is scripted.
# Useful for seeing what the opponent actually does.
watch: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish --observer --armies $(ARMIES) \
	  $(ALLIANCE_FLAG) --play $(SECONDS)

# --- Bots playing each other ------------------------------------------------
#
# `make ai-play` is the one to run while the FAF adapter is being brought up (ADR-039). It prints
# the sandbox report — what loaded, what failed and why, what the corpus called — and then plays
# a bots-vs-bots match with nobody driving, so the terminal shows both the AI's integration state
# and the opponents actually playing.
#
# The whole output is meant to be PASTED. That is why the report is one block with its own
# banner, why failures are deduplicated to distinct causes, and why module loading is traced
# line by line: the failure being chased is often a hang, and a summary printed at the end never
# arrives.
#
# The opponents playing are still the SCRIPTED ones (core/sim/BuildOrder.hpp behind ADR-038's
# port). FAF's brain does not drive an army yet, and the report is exactly the list of reasons
# why — so this target shows the gap rather than hiding it.
AI_SECONDS ?= 400

ai-play: build check-fa
	@echo
	@echo '  Bots vs bots on $(notdir $(FA_MAP)), $(ARMIES) armies, $(AI_SECONDS)s.'
	@echo '  The FAF sandbox report prints first — paste the whole block when reporting an issue.'
	@echo
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --ai-debug --skirmish --observer --armies $(ARMIES) \
	  $(ALLIANCE_FLAG) --play $(AI_SECONDS)

# The same report with no match afterwards, for a fast loop while fixing a binding. Runs the
# test binary rather than the game because it needs no map, no drive and no window.
ai-report: build
	./$(BUILD)/rm_tests '[faf]'

# A match: one army per start position, each with its faction's commander, each building an
# extractor on the map's own mass deposits.
skirmish: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish

# The same, fought. `--march` sends everything at one point and pre-runs the sim, so the
# result is the same every run — which is what makes a screenshot of it worth comparing.
battle: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish --march $(MARCH) $(SECONDS)

# --- Determinism -------------------------------------------------------------
#
# The refactor gate. `docs/golden-p1.log` is a per-tick fingerprint of one whole match —
# 5200 ticks from spawn to victory banner, covering movement, collisions, targeting,
# firing, projectiles, damage, deaths, defeats, economy, construction and the spawning of
# what gets built. `make verify` says whether the code still plays that match.
#
# WHY IT EXISTS: PLAN2.md's P1 changes how units are stored and identified, which touches
# every sim pass. Without this the only way to know a refactor changed behaviour was to
# play the game and squint. With it, a step either reports MATCH or names the tick it
# broke — which is the difference between a phase you can do in steps and one you cannot.
#
# `golden` re-records it. Only run that when the change was MEANT to alter the match, and
# say so in the commit: re-baselining silently is how a gate stops being one.
GOLDEN      ?= docs/golden-p1.log
# 700, up from 520. The golden match must cover a DECIDED match — a fingerprint of a fight
# still in progress is a weaker artifact — and P3.3 lengthened it: army 1 is Aeon and now builds
# Aeon units rather than UEF ones, whose stats differ, so the win lands at 644.9s instead of
# 502.1s.
GOLDEN_SECS ?= 700
# A screenshot is how the app stays headless — without one it opens a window and waits.
# Small and thrown away; the run is here for the hashes, not the picture.
GOLDEN_SHOT ?= $(if $(TMPDIR),$(TMPDIR),/tmp)/rm-golden.png
GOLDEN_RUN   = "$(FA_MAP)" $(FA_FLAGS) --skirmish --armies 2 --play $(GOLDEN_SECS) \
               --screenshot $(GOLDEN_SHOT) 320 240

verify: build check-fa
	$(BIN) $(GOLDEN_RUN) --check-hash-log $(GOLDEN) | grep determinism:

golden: build check-fa
	$(BIN) $(GOLDEN_RUN) --hash-log $(GOLDEN) | grep determinism:

# Milestone 20's match: a duel against the scripted opponent, played from spawn.
# Deterministic, so SECONDS picks the stage of the SAME match: 12 the first
# extractor, 60 the base, 76 the first tank, 450 the attack, 520 the banner.
match: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish --armies 2 --play $(SECONDS)

# --- Screenshots -------------------------------------------------------------
#
# Headless, so they work whichever Space is in front — the reason `--screenshot` exists.

shot-fa: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish --march $(MARCH) $(SECONDS) $(SELECT_FLAG) \
	  --screenshot $(SHOT) $(SHOT_SIZE)

# The interface, captured: a commander selected, so the build panel and the minimap are both in
# frame. The one command that shows what `core/ui/BuildPanel.hpp` actually renders.
shot-ui: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish --armies 2 --play $(SECONDS) --select 1 \
	  --screenshot $(SHOT) $(SHOT_SIZE)
	@echo "  wrote $(SHOT)"

# The same, for an ENGINEER — the unit a player actually selects to build something.
#
# `--units` is how one reaches a match at all: `data/opening.lua` builds four structures and
# then tanks, so nothing in a scripted match ever produces an engineer. The crowd spawns before
# the armies do and is adopted by the seated player once there is one (`adoptOwnerlessUnits`),
# which is what makes it selectable rather than scenery.
shot-engineer: build check-fa
	$(BIN) "$(FA_MAP)" $(FA_FLAGS) --skirmish --armies 2 --units $(FA_ENGINEER) 4 \
	  --play $(SECONDS) --select 1 --look $(MARCH) 400 --screenshot $(SHOT) $(SHOT_SIZE)
	@echo "  wrote $(SHOT)"

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
